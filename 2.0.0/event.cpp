#define DEBUG_LOG_TAG "EVENT"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include <cutils/properties.h>

#include "utils/debug.h"
#include "utils/tools.h"

#include "event.h"
#include "display.h"
#include "hwc2.h"
#include "sync.h"

#ifndef MTK_USER_BUILD
#define DEBUG_VSYNC_TIME
#endif

#define HWC_ATRACE_BUFFER(x, ...)                                               \
    if (ATRACE_ENABLED()) {                                                     \
        char ___traceBuf[256];                                                  \
        if (snprintf(___traceBuf, sizeof(___traceBuf), x, ##__VA_ARGS__) > 0) { \
            android::ScopedTrace ___bufTracer(ATRACE_TAG, ___traceBuf); }       \
    }

#define TIMEOUT_ERROR 0.1f

extern "C" int clock_nanosleep(clockid_t clock_id, int flags,
                                const struct timespec *request,
                                struct timespec *remain);

// ---------------------------------------------------------------------------

VSyncThread::VSyncThread(uint64_t dpy)
    : m_disp_id(dpy)
    , m_enabled(false)
    , m_refresh(static_cast<nsecs_t>(1e9/60))
    , m_loop(false)
    , m_fake_vsync(false)
    , m_prev_fake_vsync(0)
    , m_max_period_io(20)
    , m_max_period_req(500)
{
    m_thread_name = std::string("VSyncThread_") + std::to_string(dpy);
}

VSyncThread::~VSyncThread()
{
}

void VSyncThread::initialize(bool force_sw_vsync, nsecs_t refresh)
{
    if (force_sw_vsync)
    {
        HWC_LOGI("Force to use sw vsync");
        m_fake_vsync = true;
    }

    if (refresh > 0)
    {
        m_refresh = refresh;
        nsecs_t timeout = static_cast<nsecs_t>(TIMEOUT_ERROR * m_refresh);
        m_max_period_io = static_cast<long>(ns2ms(m_refresh + timeout));
    }

    run(m_thread_name.c_str(), PRIORITY_URGENT_DISPLAY);

    if (m_fake_vsync)
    {
        HWC_LOGD("(%" PRIu64 ") HW VSync State(%d) sw vsync period:%" PRId64 , m_disp_id, !m_fake_vsync, m_refresh);
    }
    else
    {
        HWC_LOGD("(%" PRIu64 ") HW VSync State(%d)", m_disp_id, !m_fake_vsync);
    }
}

#ifdef DEBUG_VSYNC_TIME
nsecs_t g_time_prev = systemTime();
#endif

bool VSyncThread::threadLoop()
{
    bool is_enabled = false;

    {
        Mutex::Autolock _l(m_lock);
        while (!m_enabled && !m_loop)
        {
            m_state = HWC_THREAD_IDLE;
            m_condition.wait(m_lock);
            if (exitPending()) return false;
        }

        m_state = HWC_THREAD_TRIGGER;
        is_enabled = m_enabled;
        m_loop = false;
    }

    nsecs_t next_vsync = 0;

    bool use_fake_vsync = m_fake_vsync;
    if (!use_fake_vsync)
    {
#ifdef DEBUG_VSYNC_TIME
        const nsecs_t time_prev = systemTime();
#endif

        HWC_ATRACE_NAME("wait_vsync");

        int err = HWCMediator::getInstance().getOvlDevice(m_disp_id)->waitVSync(m_disp_id, &next_vsync);
        if (err == NO_ERROR)
        {
            static nsecs_t prev_next_vsync = 0;
            HWC_ATRACE_BUFFER("period: %" PRId64, next_vsync - prev_next_vsync)
            prev_next_vsync = next_vsync;
#ifdef DEBUG_VSYNC_TIME
            const nsecs_t time_curr = systemTime();
            const nsecs_t dur1 = time_curr - g_time_prev;
            const nsecs_t dur2 = time_curr - time_prev;
            g_time_prev = time_curr;
            long p1 = static_cast<long>(ns2ms(dur1));
            long p2 = static_cast<long>(ns2ms(dur2));
            {
                Mutex::Autolock _l(m_lock);
                if (p1 > m_max_period_req || p2 > m_max_period_io)
                    HWC_LOGD("vsync/dpy=%" PRIu64 "/req=%ld/io=%ld", m_disp_id, p1, p2);
            }
#endif
        }
        else
        {
            // use fake vsync since fail to get hw vsync
            use_fake_vsync = true;
        }
    }

    if (use_fake_vsync)
    {
        nsecs_t period = 0;
        {
            Mutex::Autolock _l(m_lock);
            period = m_refresh;
        }
        const nsecs_t now = systemTime(CLOCK_MONOTONIC);
        next_vsync = m_prev_fake_vsync + period;
        nsecs_t sleep = next_vsync - now;

        if (sleep < 0)
        {
            sleep = (period - ((now - next_vsync) % period));
            next_vsync = now + sleep;
        }

        struct timespec spec;
        spec.tv_sec  = static_cast<long>(next_vsync / 1000000000);
        spec.tv_nsec = next_vsync % 1000000000;


        HWC_LOGD("(%" PRIu64 ") use SW VSync sleep: %.2f ms", m_disp_id, sleep / 1000000.0);
        int err;
        do {

            err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
        } while (err<0 && errno == EINTR);

        m_prev_fake_vsync = next_vsync;
    }

    DisplayManager::getInstance().vsync(m_disp_id, next_vsync, is_enabled);

    return true;
}

void VSyncThread::setEnabled(bool enabled)
{
    Mutex::Autolock _l(m_lock);

#ifdef DEBUG_VSYNC_TIME
    if (m_enabled != enabled)
        HWC_LOGD("vsync/dpy=%" PRIu64 "/en=%d", m_disp_id, enabled);
#endif

    m_enabled = enabled;
    m_condition.signal();
}

void VSyncThread::setLoopAgain()
{
    Mutex::Autolock _l(m_lock);

#ifdef DEBUG_VSYNC_TIME
    HWC_LOGD("Set Loop Again");
#endif

    m_loop = true;
    m_condition.signal();
}

void VSyncThread::setProperty()
{
    char value[PROPERTY_VALUE_MAX] = {0};

    property_get("vendor.debug.sf.sw_vsync_fps", value, "0");
    int fps = atoi(value);
    if (fps > 0)
    {
        m_refresh = nsecs_t(1e9 / fps);
        HWC_LOGD("Set sw vsync fps(%d), period(%" PRId64 ")", fps, m_refresh);
    }

    property_get("vendor.debug.hwc.period_io", value, "0");
    if (atoi(value))
    {
        m_max_period_io = atoi(value);
        HWC_LOGD("Set max checking period_io(%ld)", m_max_period_io);
    }

    property_get("vendor.debug.hwc.period_req", value, "0");
    if (atoi(value))
    {
        m_max_period_req = atoi(value);
        HWC_LOGD("Set max checking period_req(%ld)", m_max_period_io);
    }
}

void VSyncThread::updatePeriod(nsecs_t period)
{
    if (CC_UNLIKELY(period != m_refresh) && CC_LIKELY(period > 0))
    {
        Mutex::Autolock _l(m_lock);
        m_refresh = period;
        nsecs_t timeout = static_cast<nsecs_t>(TIMEOUT_ERROR * m_refresh);
        m_max_period_io = static_cast<long>(ns2ms(m_refresh + timeout));
        HWC_LOGD("(%" PRIu64 ") update sw vsync period:%" PRId64 " timeout:%" PRIu64,
                m_disp_id, m_refresh, timeout);
    }
}

// ---------------------------------------------------------------------------
#define UEVENT_BUFFER_SIZE 2048

UEventThread::UEventThread()
    : m_socket(-1)
    , m_is_hotplug(false)
    , m_fake_hdmi(FAKE_HDMI_NONE)
    , m_fake_hotplug(false)
{
}

UEventThread::~UEventThread()
{
    if (m_socket > 0) ::protectedClose(m_socket);
}

void UEventThread::initialize()
{
    struct sockaddr_nl addr;
    int optval = 64 * 1024;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = static_cast<__u32>(getpid());
    addr.nl_groups = 0xffffffff;

    m_socket = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (m_socket < 0)
    {
        HWC_LOGE("Failed to create socket");
        return;
    }

    if ((setsockopt(m_socket, SOL_SOCKET, SO_RCVBUFFORCE, &optval, sizeof(optval)) < 0) &&
        (setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval)) < 0))
    {
        HWC_LOGE("Failed to set SO_RCVBUFFORCE/SO_RCVBUF option on socket");
        return;
    }

    if (bind(m_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        HWC_LOGE("Failed to bind socket");
        return;
    }

    run("UEventThreadHWC", PRIORITY_URGENT_DISPLAY);

    HWC_LOGW("Start to listen uevent, addr.nl_pid(%d)", addr.nl_pid);
}

void UEventThread::handleUevents(const char *buff, ssize_t len)
{
    const char *s = buff;
    const int change_hdmi = !strcmp(s, "change@/devices/virtual/switch/hdmi");
    const int change_hdmi_res = !strcmp(s, "change@/devices/virtual/switch/res_hdmi");
    const int change_widevine = !strcmp(s, "change@/devices/virtual/switch/widevine");

    if (!change_hdmi && !change_hdmi_res && !change_widevine)
       return;

    HWC_LOGD("handle hdmi uevents: s=%p, len=%zd", s, len);

    int state = 0;
    s += strlen(s) + 1;

    while (*s)
    {
        if (!strncmp(s, "SWITCH_STATE=", strlen("SWITCH_STATE=")))
        {
            state = atoi(s + strlen("SWITCH_STATE="));
            HWC_LOGD("uevents: SWITCH_STATE=%d", state);
        }

        HWC_LOGD("uevents: s=%p, %s", s, s);

        s += strlen(s) + 1;
        if (s - buff >= len)
            break;
    }

    if (change_hdmi)
    {
        if (state == 0x1)
        {
            HWC_LOGD("uevents: hdmi connecting...");
            DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, true);
            m_is_hotplug = true;
        }
        else
        {
            HWC_LOGD("uevents: hdmi disconnect");

            if (m_is_hotplug)
            {
                DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, false);
            }

            m_is_hotplug  = false;
        }
    }
    else if (change_hdmi_res)
    {
        if (state != 0x0)
        {
            if (m_is_hotplug)
            {
                HWC_LOGD("uevents: disconnect before reconnect hdmi");
                DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, false);
                usleep(32000);
            }

            HWC_LOGD("uevents: change hdmi resolution");
            DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, true);
            m_is_hotplug = true;
        }
    }
    else if (change_widevine)
    {
        HWC_LOGI("uevents: video hdcp version(%d)", state);
        DisplayManager::getInstance().setVideoHdcp(static_cast<uint32_t>(state));
    }
    else
    {
        HWC_LOGE("unknown uevents");
    }
}

bool UEventThread::threadLoop()
{
    struct pollfd fds;
    static char uevent_desc[UEVENT_BUFFER_SIZE * 2];

    fds.fd = m_socket;
    fds.events = POLLIN;
    fds.revents = 0;
    int ret = poll(&fds, 1, -1);

    if (ret > 0 && (fds.revents & POLLIN))
    {
        /* keep last 2 zeroes to ensure double 0 termination */
        ssize_t count = recv(m_socket, uevent_desc, sizeof(uevent_desc) - 2, 0);
        if (count > 0) handleUevents(uevent_desc, count);
    }

    if (FAKE_HDMI_PLUG == m_fake_hdmi)
    {
        if (m_is_hotplug)
        {
            HWC_LOGD("Disconnect hdmi before reconnect");

            DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, false);
            usleep(32000);
        }

        HWC_LOGD("FAKE_HDMI_PLUG !!");

        DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, true, true);
        m_fake_hotplug = true;
    }
    else if ((FAKE_HDMI_UNPLUG == m_fake_hdmi) && m_fake_hotplug)
    {
        HWC_LOGD("FAKE_HDMI_UNPLUG !!");

        DisplayManager::getInstance().hotplugExt(HWC_DISPLAY_EXTERNAL, false, true);
        m_fake_hotplug = false;
    }

    m_fake_hdmi = FAKE_HDMI_NONE;

    return true;
}

void UEventThread::setProperty()
{
    char value[PROPERTY_VALUE_MAX] = {0};

    property_get("vendor.debug.hwc.test_hdmi_connect", value, "0");
    m_fake_hdmi = atoi(value);

    HWC_LOGD("Connect fake hdmi(%d)", m_fake_hdmi);
}

RefreshRequestThread::RefreshRequestThread()
    : m_enabled(false)
{
}

RefreshRequestThread::~RefreshRequestThread()
{
}

void RefreshRequestThread::initialize()
{
    run("RefreshRequestThread", PRIORITY_URGENT_DISPLAY);
}

void RefreshRequestThread::setEnabled(bool enable)
{
    Mutex::Autolock _l(m_lock);
    m_enabled = enable;
    m_condition.signal();
}

bool RefreshRequestThread::threadLoop()
{
    {
        Mutex::Autolock _l(m_lock);
        if (exitPending()) {
            return false;
        }
        while (!m_enabled)
        {
            m_state = HWC_THREAD_IDLE;
            m_condition.wait(m_lock);
            if (exitPending()) {
                return false;
            }
        }
        m_state = HWC_THREAD_TRIGGER;
    }

    unsigned int type;
    int err = getHwDevice()->waitRefreshRequest(&type);
    if (err == NO_ERROR) {
        DisplayManager::getInstance().refreshForDriver(type);
    }

    return true;
}

IdleThread::IdleThread(uint64_t dpy)
    : m_enabled(false)
    , m_wait_time(0)
{
    m_thread_name = std::string("IdleThread_") + std::to_string(dpy);
}

IdleThread::~IdleThread()
{
}

void IdleThread::initialize(nsecs_t refresh)
{
    m_wait_time = refresh;
    run(m_thread_name.c_str(), PRIORITY_URGENT_DISPLAY);
}

void IdleThread::setEnabled(bool enable)
{
    Mutex::Autolock _l(m_lock);
    m_enabled = enable;
    m_condition.signal();
}

bool IdleThread::threadLoop()
{
    Mutex::Autolock _l(m_lock);
    if (exitPending()) {
        return false;
    }
    while (!m_enabled)
    {
        m_state = HWC_THREAD_IDLE;
        m_condition.wait(m_lock);
        if (exitPending()) {
            return false;
        }
    }
    m_state = HWC_THREAD_TRIGGER;

    if (m_condition.waitRelative(m_lock, m_wait_time) == TIMED_OUT){
        HWC_ATRACE_NAME("idle_refresh");
        HWC_LOGD("idle_refresh");
        DisplayManager::getInstance().refreshForDisplay(HWC_DISPLAY_PRIMARY,
                HWC_REFRESH_FOR_IDLE_THREAD);
        m_enabled = false;
    }

    return true;
}

HWVSyncEstimator& HWVSyncEstimator::getInstance()
{
    static HWVSyncEstimator gInstance;
    return gInstance;
}

HWVSyncEstimator::HWVSyncEstimator()
    : m_sample_count(0)
    , m_last_signaled_prenset_fence_time(0)
{
    resetAvgVSyncPeriod(DisplayManager::getInstance().getDisplayData(HWC_DISPLAY_PRIMARY)->refresh);
}

HWVSyncEstimator::~HWVSyncEstimator()
{
    resetAvgVSyncPeriod(0);
}

void HWVSyncEstimator::resetAvgVSyncPeriod(nsecs_t period)
{
    AutoMutex l(m_mutex);
    for (int fence_fd : m_present_fence_queue)
    {
        ::protectedClose(fence_fd);
    }
    m_present_fence_queue.clear();
    m_avg_period = period;
    m_cur_config_period = -1;
}

void HWVSyncEstimator::pushPresentFence(const int& fd, const nsecs_t cur_period)
{
    AutoMutex l(m_mutex);
    if (m_cur_config_period != -1 && m_cur_config_period != cur_period)
    {
        HWC_LOGW("period are changed without resetAvgVSyncPeriod");
    }
    m_cur_config_period = cur_period;

    m_present_fence_queue.push_back(fd);
    if (m_present_fence_queue.size() > m_history_size)
    {
        std::list<int>::iterator it = m_present_fence_queue.begin();
        ::protectedClose(*it);
        m_present_fence_queue.erase(it);
    }
}

nsecs_t HWVSyncEstimator::getNextHWVsync(nsecs_t cur)
{
    AutoMutex l(m_mutex);
    updateLocked();
    if (m_last_signaled_prenset_fence_time <= 0)
    {
        return -1;
    }

    return m_last_signaled_prenset_fence_time +
            (((cur - m_last_signaled_prenset_fence_time) / m_avg_period) + 1) * m_avg_period;
}

void HWVSyncEstimator::update()
{
    AutoMutex l(m_mutex);
    updateLocked();
}

void HWVSyncEstimator::updateLocked()
{
    nsecs_t cur_fence_signal_time = 0, prev_fence_signal_time = 0;
    std::list<int>::iterator it;
    std::list<int>::iterator last_signaled = m_present_fence_queue.begin();

    if (CC_UNLIKELY(m_cur_config_period <= 0))
    {
        HWC_LOGW("m_cur_config_period <= 0");
        m_cur_config_period = DisplayManager::getInstance().getDisplayData(HWC_DISPLAY_PRIMARY)->refresh;
    }

    for (it = m_present_fence_queue.begin(); it != m_present_fence_queue.end(); ++it)
    {
        nsecs_t temp = getFenceSignalTime(*it);
        if (temp != SIGNAL_TIME_INVALID && temp != SIGNAL_TIME_PENDING) {
            prev_fence_signal_time = cur_fence_signal_time;
            cur_fence_signal_time = temp;
            last_signaled = it;

            if (it != m_present_fence_queue.begin())
            {
                uint32_t num_of_vsync =
                    static_cast<uint32_t>((cur_fence_signal_time - prev_fence_signal_time) /
                                          static_cast<float>(m_cur_config_period) + 0.5f);

                if (m_sample_count < 100)
                    m_sample_count++;

                if (num_of_vsync > 0 && num_of_vsync < 5)
                    m_avg_period = (((m_sample_count - 1) * m_avg_period) +
                                    ((cur_fence_signal_time - prev_fence_signal_time) / num_of_vsync))
                                    / m_sample_count;
            }
        }
    }

    if (last_signaled != m_present_fence_queue.begin())
    {
        for (it = m_present_fence_queue.begin(); it != last_signaled; ++it) {
            ::protectedClose(*it);
        }
        m_present_fence_queue.erase(m_present_fence_queue.begin(), last_signaled);
    }
    m_last_signaled_prenset_fence_time = cur_fence_signal_time;
}

FenceDebugger::FenceDebugger(std::string name, int mode, bool wait_log)
    : m_name(name)
    , m_mode(mode)
    , m_wait_log(wait_log)
{
}

FenceDebugger::~FenceDebugger()
{
}

void FenceDebugger::initialize()
{
    run(m_name.c_str(), PRIORITY_URGENT_DISPLAY);
}

void FenceDebugger::dupAndStoreFence(const int fd, const unsigned int fence_idx)
{
    if (fd < 0)
    {
        HWC_LOGW("%s(), fd %d < 0", __FUNCTION__, fd);
        HWC_ATRACE_NAME("warning, fd < 0");
        return;
    }

    {
        Mutex::Autolock _l(m_lock);

        m_fence_queue.push_back({ .fd = ::dup(fd), .fence_idx = fence_idx});
        m_condition.signal();
    }

    if (m_fence_queue.size() > 20)
    {
        HWC_LOGW("%s(), fd queue size %zu > 20", __FUNCTION__, m_fence_queue.size());
        HWC_ATRACE_NAME("warning, fence queue size > 20");
    }
}


bool FenceDebugger::threadLoop()
{
    FenceInfo cur_f_info = { .fd = -1, .fence_idx = UINT_MAX };
    {
        Mutex::Autolock _l(m_lock);
        if (exitPending()) {
            return false;
        }

        if (m_fence_queue.empty())
        {
            m_state = HWC_THREAD_IDLE;
            m_condition.wait(m_lock);
            return true;
        }

        m_state = HWC_THREAD_TRIGGER;

        cur_f_info = m_fence_queue.front();
        m_fence_queue.pop_front();
    }

    if (cur_f_info.fd == -1)
    {
        HWC_LOGW("cur_f_info.id = -1");
        return true;
    }

    std::string dbg_name = m_name +
                           std::string("_") + std::to_string(cur_f_info.fd) +
                           std::string("_") + std::to_string(cur_f_info.fence_idx);

    if (m_wait_log)
    {
        HWC_LOGI("%s, wait+", dbg_name.c_str());
    }

    int ret = ((m_mode & WAIT_PERIODICALLY) != 0) ?
              SyncFence::waitPeriodicallyWoCloseFd(cur_f_info.fd, 100, 1000, dbg_name.c_str()) :
              SyncFence::waitWithoutCloseFd(cur_f_info.fd, 1500, dbg_name.c_str());

    if (ret == 0)
    {
        uint64_t signal_time = SyncFence::getSignalTime(cur_f_info.fd);
        if (signal_time == m_prev_signal_time)
        {
            HWC_LOGI("2 same signal time %" PRId64, signal_time);
            HWC_ATRACE_NAME((dbg_name + std::string(" 2 same ts ") + std::to_string(signal_time)).c_str());
            HWC_ATRACE_INT((m_name + "_same_ts").c_str(), m_same_count++ % 2);
        }

        if ((m_mode & CHECK_DIFF_BIG) != 0)
        {
            int64_t diff = static_cast<int64_t>(signal_time - m_prev_signal_time);
            HWC_ATRACE_INT64(diff > 50 * 1000 * 1000 ?
                             (m_name + "_diff_big").c_str() : (m_name + "_diff").c_str(),
                             diff);
        }

        m_prev_signal_time = signal_time;

        HWC_ATRACE_NAME((dbg_name + std::string(" signal ts ") + std::to_string(signal_time)).c_str());
    }
    else
    {
        HWC_ATRACE_NAME((dbg_name + std::string(" wait fence timeout")).c_str());
    }

    if (m_wait_log)
    {
        HWC_LOGI("%s, wait-, ret %d", dbg_name.c_str(), ret);
    }

    ::protectedClose(cur_f_info.fd);

    return true;
}

