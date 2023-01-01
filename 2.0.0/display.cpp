#define DEBUG_LOG_TAG "DPY"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <fcntl.h>
#include <stdint.h>

#include <cutils/properties.h>

#include <hwc_feature_list.h>

#include "utils/debug.h"
#include "utils/tools.h"

#include "dispatcher.h"
#include "display.h"
#include "overlay.h"
#include "event.h"
#include "sync.h"
#include "hwc2.h"

#include "platform_wrap.h"

#include <ged/ged_dvfs.h>
#include <ged/ged.h>

#define BOUNDARY_CHECK_DPY(dpy)                                                                 \
    if (dpy >= DisplayManager::MAX_DISPLAYS) {                                       \
        LOG_FATAL("[%s] dpy(%d) out of range %s:%d", DEBUG_LOG_TAG, dpy, __func__, __LINE__);   \
    }

// ---------------------------------------------------------------------------

sp<UEventThread> g_uevent_thread = NULL;
sp<RefreshRequestThread> g_refresh_thread = NULL;

#ifdef MTK_USER_BUILD
int DisplayManager::m_profile_level = PROFILE_NONE;
#else
int DisplayManager::m_profile_level = PROFILE_COMP | PROFILE_BLT | PROFILE_TRIG;
#endif

DisplayManager& DisplayManager::getInstance()
{
    static DisplayManager gInstance;
    return gInstance;
}

DisplayManager::DisplayManager()
    : m_data_primary_extra(nullptr)
    , m_curr_disp_num(0)
    , m_fake_disp_num(0)
    , m_listener(NULL)
    , m_video_hdcp(UINT_MAX)
    , m_ext_active(false)
    , m_ged_log_handle(nullptr)
{
    m_data = (DisplayData*) calloc(1, MAX_DISPLAYS * sizeof(DisplayData));
    LOG_ALWAYS_FATAL_IF(m_data == nullptr, "DisplayManager::m_data calloc(%zu) fail",
        MAX_DISPLAYS * sizeof(DisplayData));

    m_data[HWC_DISPLAY_PRIMARY].trigger_by_vsync = false;
    m_data[HWC_DISPLAY_EXTERNAL].trigger_by_vsync = false;
    m_data[HWC_DISPLAY_VIRTUAL].trigger_by_vsync = false;
    switch (HwcFeatureList::getInstance().getFeature().trigger_by_vsync)
    {
        case 1:
            m_data[HWC_DISPLAY_EXTERNAL].trigger_by_vsync = true;
            break;
        case 2:
            m_data[HWC_DISPLAY_EXTERNAL].trigger_by_vsync = true;
            m_data[HWC_DISPLAY_VIRTUAL].trigger_by_vsync = true;
            break;
    }

    g_uevent_thread = new UEventThread();
    if (g_uevent_thread == NULL)
    {
        HWC_LOGE("Failed to initialize UEvent thread!!");
        abort();
    }
    g_uevent_thread->initialize();

    if (HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY)->isDispSelfRefreshSupported())
    {
        g_refresh_thread = new RefreshRequestThread();
        if (g_refresh_thread == NULL) {
            HWC_LOGE("Failed to initialize RefreshRequestThread");
            abort();
        }
        g_refresh_thread->initialize();
        g_refresh_thread->setEnabled(true);
    }

    memset(m_state_ultra_display, 0, sizeof(m_state_ultra_display));
    for (int i = 0; i < MAX_DISPLAYS; i++)
    {
        m_display_power_state[i] = false;
        m_usage[i] = ComposerExt::DisplayUsage::kUnknown;
    }

    for (size_t i = 0; i < MAX_DISPLAYS; i++)
    {
        m_first_commit_done[i] = false;
    }

    m_primay_display_power = HWC2_POWER_MODE_ON;
}

DisplayManager::~DisplayManager()
{
    m_listener = NULL;
    free(m_data);
    free(m_data_primary_extra);

    if (m_ged_log_handle != nullptr)
        ged_log_disconnect(m_ged_log_handle);
}

void DisplayManager::init()
{
    m_curr_disp_num = 1;

    if (m_listener != NULL) m_listener->onPlugIn(HWC_DISPLAY_PRIMARY);

    createVsyncThread(HWC_DISPLAY_PRIMARY);

    HWC_LOGI("Display Information:");
    HWC_LOGI("# fo current devices : %d", m_curr_disp_num.load(memory_order_relaxed));
    for (unsigned int i = 0; i < m_curr_disp_num; i++)
    {
        printDisplayInfo(i);
        m_display_power_state[i] = true;
    }

    m_listener->onHotPlugExt(HWC_DISPLAY_PRIMARY, true);
}

void DisplayManager::resentCallback()
{
    m_listener->onHotPlugExt(HWC_DISPLAY_PRIMARY, true);
}

void DisplayManager::createVsyncThread(uint64_t dpy)
{
    BOUNDARY_CHECK_DPY(dpy);
    AutoMutex _l(m_vsyncs[dpy].lock);
    m_vsyncs[dpy].thread = new VSyncThread(dpy);
    if (m_vsyncs[dpy].thread == NULL)
    {
        HWC_LOGE("dpy=%" PRIu64 "/Failed to initialize VSYNC thread!!", dpy);
        abort();
    }
    m_vsyncs[dpy].thread->initialize(!m_data[dpy].has_vsync, m_data[dpy].refresh);
}

void DisplayManager::destroyVsyncThread(uint64_t dpy)
{
    BOUNDARY_CHECK_DPY(dpy);
    if (m_vsyncs[dpy].thread != NULL)
    {
        m_vsyncs[dpy].thread->requestExit();
        m_vsyncs[dpy].thread->setLoopAgain();
        m_vsyncs[dpy].thread->join();
    }

    {
        // We cannot lock the whole destoryVsyncThread(), or it will cause the deadlock between
        // UEventThread and DispatcherThread. When a secondary display plugged out, UEventThread
        // will wait completion of VSyncThread, and needs to acquire the VSync lock of onVSync().
        // DispatcherThread acquired the lock of onVsync() firstly and request a next VSync.
        // Unfortunately, DispatcherThread cannot get a VSync because the vsync lock is acquired
        // by UEventThread.
        AutoMutex _l(m_vsyncs[dpy].lock);
        m_vsyncs[dpy].thread = NULL;
    }
}

void DisplayManager::printDisplayInfo(uint64_t dpy)
{
    if (dpy >= MAX_DISPLAYS) return;

    DisplayData* disp_data = &m_data[dpy];

    HWC_LOGI("------------------------------------");
    HWC_LOGI("Device id   : %" PRIu64,   dpy);
    HWC_LOGI("Width       : %u",   disp_data->width);
    HWC_LOGI("Height      : %u",   disp_data->height);
    HWC_LOGI("xdpi        : %f",   disp_data->xdpi);
    HWC_LOGI("ydpi        : %f",   disp_data->ydpi);
    HWC_LOGI("vsync       : %d",   disp_data->has_vsync);
    HWC_LOGI("refresh     : %" PRId64, disp_data->refresh);
    HWC_LOGI("connected   : %d",   disp_data->connected);
    HWC_LOGI("hwrotation  : %d",   disp_data->hwrotation);
    HWC_LOGI("subtype     : %d",   disp_data->subtype);
    HWC_LOGI("secure      : %d",   disp_data->secure ? 1 : 0);
    HWC_LOGI("aspect      : %1.3f, %1.3f",
        disp_data->aspect_portrait, disp_data->aspect_landscape);
    HWC_LOGI("portrait    : [%4d,%4d,%4d,%4d]",
        disp_data->mir_portrait.left,  disp_data->mir_portrait.top,
        disp_data->mir_portrait.right, disp_data->mir_portrait.bottom);
    HWC_LOGI("landscape   : [%4d,%4d,%4d,%4d]",
        disp_data->mir_landscape.left,  disp_data->mir_landscape.top,
        disp_data->mir_landscape.right, disp_data->mir_landscape.bottom);
    HWC_LOGI("trigger_by_vsync: %d", disp_data->trigger_by_vsync);
    HWC_LOGI("density     : %d", disp_data->density);
}

void DisplayManager::dump(struct dump_buff* /*log*/)
{
    if (m_vsyncs[HWC_DISPLAY_PRIMARY].thread != NULL)
        m_vsyncs[HWC_DISPLAY_PRIMARY].thread->setProperty();

    if (g_uevent_thread != NULL)
        g_uevent_thread->setProperty();
}

void DisplayManager::setListener(const sp<EventListener>& listener)
{
    m_listener = listener;
}

void DisplayManager::requestVSync(uint64_t dpy, bool enabled)
{
    BOUNDARY_CHECK_DPY(dpy);
    AutoMutex _l(m_vsyncs[dpy].lock);
    if (m_vsyncs[dpy].thread != NULL)
        m_vsyncs[dpy].thread->setEnabled(enabled);
}

void DisplayManager::requestNextVSync(uint64_t dpy)
{
    BOUNDARY_CHECK_DPY(dpy);
    AutoMutex _l(m_vsyncs[dpy].lock);
    if (m_vsyncs[dpy].thread != NULL)
        m_vsyncs[dpy].thread->setLoopAgain();
}

void DisplayManager::vsync(uint64_t dpy, nsecs_t timestamp, bool enabled)
{
    if (m_listener != NULL)
    {
        // check if primary display needs to use external vsync source
        if (HWC_DISPLAY_PRIMARY != dpy)
        {
            Mutex::Autolock _l(m_power_lock);

            if (m_data[HWC_DISPLAY_PRIMARY].vsync_source == dpy)
                m_listener->onVSync(HWC_DISPLAY_PRIMARY, timestamp, enabled);
        }

        m_listener->onVSync(dpy, timestamp, enabled);
    }
}

void DisplayManager::hotplugExt(uint64_t dpy, bool connected, bool fake, bool notify)
{
    if (dpy != HWC_DISPLAY_EXTERNAL)
    {
        HWC_LOGW("Failed to hotplug external disp(%" PRIu64 ") connect(%d) !", dpy, connected);
        return;
    }

    HWC_LOGI("Hotplug external disp(%" PRIu64 ") connect(%d) fake(%d)", dpy, connected, fake);

    DisplayData* disp_data = &m_data[HWC_DISPLAY_EXTERNAL];

    disp_data->trigger_by_vsync =
        HwcFeatureList::getInstance().getFeature().trigger_by_vsync > 0 ? true : false;

    if (connected && !disp_data->connected)
    {
        sp<IOverlayDevice> ovlDevice = HWCMediator::getInstance().getOvlDevice(dpy);
        int32_t res = ovlDevice->updateDisplayResolution(HWC_DISPLAY_EXTERNAL);
        if (res != NO_ERROR)
        {
            HWC_LOGE("give up report hotplug event: %d", res);
            return;
        }

        HWCMediator::getInstance().createExternalDisplay();
        if (m_listener != NULL) m_listener->onPlugIn(dpy);

        if (fake == true)
        {
            static int _s_shrink_size = 4;
            _s_shrink_size = (_s_shrink_size == 2) ? 4 : 2;
            memcpy(disp_data, &m_data[0], sizeof(DisplayData));
            disp_data->width   = m_data[0].width / _s_shrink_size;
            disp_data->height  = m_data[0].height / _s_shrink_size;
            disp_data->subtype = FAKE_DISPLAY;

            m_fake_disp_num++;
        }

        createVsyncThread(dpy);

        hotplugPost(dpy, 1, DISP_PLUG_CONNECT, notify);

        if (m_listener != NULL && notify) m_listener->onHotPlugExt(dpy, HWC2_CONNECTION_CONNECTED);

        if (disp_data->trigger_by_vsync)
        {
            requestVSync(dpy, true);
        }
        HWCDispatcher::getInstance().ignoreJob(dpy, false);
        status_t ret = m_condition.waitRelative(m_uevent_lock, ms2ns(3000));
        if (ret != NO_ERROR)
        {
            if (ret == TIMED_OUT)
            {
                HWC_LOGW("hotplug in is still not finished");
            }
            else
            {
                HWC_LOGE("To wait hotplug in failed(%d)", ret);
            }
        }
        HWC_LOGI("---external display hotplug in end---");
    }
    else if (!connected && disp_data->connected)
    {
        HWCDispatcher::getInstance().ignoreJob(dpy, true);

        if (fake == true)
        {
            if (m_fake_disp_num < 1)
            {
                HWC_LOGW("%s(), m_fake_disp_num %u < 1", __FUNCTION__, m_fake_disp_num);
                m_fake_disp_num = 1;
            }
            m_fake_disp_num--;
        }

        if (disp_data->trigger_by_vsync)
        {
            requestVSync(dpy, false);
        }

        destroyVsyncThread(dpy);

        if (m_listener != NULL)
        {
            m_listener->onPlugOut(dpy);
            if (notify)
            {
                m_listener->onHotPlugExt(dpy, HWC2_CONNECTION_DISCONNECTED);
            }
        }

        status_t ret = m_condition.waitRelative(m_uevent_lock, ms2ns(3000));
        if (ret != NO_ERROR)
        {
            if (ret == TIMED_OUT)
            {
                HWC_LOGW("hotplug out is still not finished");
            }
            else
            {
                HWC_LOGE("To wait hotplug out failed(%d)", ret);
            }
        }
        HWCMediator::getInstance().destroyExternalDisplay();
        HWC_LOGI("---external display hotplug out end---");
    }
}

void DisplayManager::hotplugExtOut()
{
    hotplugPost(HWC_DISPLAY_EXTERNAL, 0, DISP_PLUG_DISCONNECT, true);
}

void DisplayManager::notifyHotplugInDone()
{
    if (!m_ext_active)
    {
        m_ext_active = true;
        m_condition.signal();
    }
}

void DisplayManager::notifyHotplugOutDone()
{
    if (m_ext_active)
    {
        m_ext_active = false;
        m_condition.signal();
    }
}

void DisplayManager::hotplugVir(
    const uint64_t& dpy,
    const bool& connected,
    const uint32_t& width,
    const uint32_t& height,
    const unsigned int& format)
{
    if (dpy != HWC_DISPLAY_VIRTUAL)
    {
        HWC_LOGW("Failed to hotplug virtual disp(%" PRIu64 ") !", dpy);
        return;
    }

    DisplayData* disp_data = &m_data[HWC_DISPLAY_VIRTUAL];

    disp_data->trigger_by_vsync =
        HwcFeatureList::getInstance().getFeature().trigger_by_vsync > 1 ? true : false;

    HWC_LOGW("Hotplug virtual disp(%" PRIu64 ") connect(%d)", dpy, connected);

    if (connected)
    {
        setDisplayDataForVir(HWC_DISPLAY_VIRTUAL, width, height, format);

        hotplugPost(dpy, 1, DISP_PLUG_CONNECT);

        if (m_listener != NULL) m_listener->onPlugIn(dpy, width, height);

        // TODO: How HWC receives requests from WFD frameworks
    }
    else
    {
        if (m_listener != NULL) m_listener->onPlugOut(dpy);

        hotplugPost(dpy, 0, DISP_PLUG_DISCONNECT);

        // TODO: How HWC receives requests from WFD frameworks
    }
}

bool DisplayManager::checkIsWfd(uint64_t dpy)
{
    bool is_wfd = false;
    bool usage_is_wfd = (static_cast<uint32_t>(getUsage(dpy)) & static_cast<uint32_t>(ComposerExt::DisplayUsage::kIsWFD)) > 0;
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get("debug.sf.enable_hwc_vds", value, "-1");
    int enable_hwc_vds = atoi(value);
    if (enable_hwc_vds == 0)
    {
        // debug.sf.enable_hwc_vds is set to 0, so SurfaceFlinger should not
        // request HWC to process virtual display. the exception is that
        // SurfaceFlinger has applied the patch to check virtaul display
        // whether it is WFD. therefore we assume SurfaceFlinger has checked
        // the virtual display when this property is 0.
        is_wfd = true;
    }
    else
    {
        is_wfd = usage_is_wfd;
    }
    HWC_LOGI("%s: is_wfd=%d enable_hwc_vds[%d] usage_is_wfd[%d]",
            __func__, is_wfd, enable_hwc_vds, usage_is_wfd);
    return is_wfd;
}

void DisplayManager::refreshForDisplay(uint64_t dpy, unsigned int type)
{
    if (m_listener != NULL)
    {
        m_listener->onRefresh(dpy, type);
    }
}

void DisplayManager::refreshForDriver(unsigned int type)
{
    if (m_listener != NULL) {
        if (HWC_WAIT_FOR_REFRESH < type && type < HWC_REFRESH_TYPE_NUM) {
            m_listener->onRefresh(HWC_DISPLAY_PRIMARY, type);
        }
    }
}

void DisplayManager::setDisplayDataForVir(const uint64_t& dpy,
                                          const uint32_t& width, const uint32_t& height,
                                          const unsigned int& format)
{
    DisplayData* disp_data = &m_data[dpy];

    if (dpy == HWC_DISPLAY_VIRTUAL)
    {
        disp_data->width     = static_cast<int>(width);
        disp_data->height    = static_cast<int>(height);
        disp_data->format    = format;
        disp_data->xdpi      = m_data[HWC_DISPLAY_PRIMARY].xdpi;
        disp_data->ydpi      = m_data[HWC_DISPLAY_PRIMARY].ydpi;
        disp_data->has_vsync = false;
        disp_data->connected = true;
        disp_data->density   = m_data[HWC_DISPLAY_PRIMARY].density;

        const bool is_wfd = checkIsWfd(dpy);
        if (is_wfd)
        {
            disp_data->secure  = (static_cast<uint32_t>(getUsage(dpy)) & static_cast<uint32_t>(ComposerExt::DisplayUsage::kIsSecure)) > 0;
            disp_data->subtype = HWC_DISPLAY_WIRELESS;
            disp_data->hdcp_version = UINT_MAX;
            HWC_LOGI("(%" PRIu64 ") hdcp version is %d", dpy, disp_data->hdcp_version);
        }
        else
        {
            disp_data->secure  = false;
            disp_data->subtype = HWC_DISPLAY_MEMORY;
        }

        // [NOTE]
        // only if the display without any physical rotation,
        // same ratio can be applied to both portrait and landscape
        disp_data->aspect_portrait  = float(disp_data->width) / float(disp_data->height);
        disp_data->aspect_landscape = disp_data->aspect_portrait;

        // TODO
        //disp_data->vsync_source = HWC_DISPLAY_VIRTUAL;

        // currently no need for vir disp
        disp_data->hwrotation = 0;

        disp_data->pixels = static_cast<unsigned int>(disp_data->width * disp_data->height);

        disp_data->trigger_by_vsync =
            HwcFeatureList::getInstance().getFeature().trigger_by_vsync > 1 ? true : false;
    }
}

void DisplayManager::setDisplayDataForPhy(uint64_t dpy, buffer_handle_t /*outbuf*/)
{
    DisplayData* disp_data = &m_data[dpy];

    char value[PROPERTY_VALUE_MAX] = {0};

    // give default value of density
    float density = 160.0;

    if (dpy == HWC_DISPLAY_PRIMARY)
    {
        SessionInfo info;
        sp<IOverlayDevice> ovlDevice = HWCMediator::getInstance().getOvlDevice(dpy);
        ovlDevice->getOverlaySessionInfo(HWC_DISPLAY_PRIMARY, &info);

        disp_data->width     = static_cast<int>(info.displayWidth);
        disp_data->height    = static_cast<int>(info.displayHeight);
        disp_data->format    = info.displayFormat;
        unsigned int physicalWidth = 0;
        unsigned int physicalHeight = 0;
        getPhysicalPanelSize(&physicalWidth, &physicalHeight, info);
        disp_data->density   = info.density == 0 ? static_cast<int>(density) : static_cast<int>(info.density);
        disp_data->xdpi      = physicalWidth == 0 ? disp_data->density * 1000:
                                (info.displayWidth * 25.4f * 1000000 / physicalWidth);
        disp_data->ydpi      = physicalHeight == 0 ? disp_data->density * 1000:
                                (info.displayHeight * 25.4f * 1000000 / physicalHeight);

        disp_data->has_vsync = info.isHwVsyncAvailable;
        disp_data->connected = info.isConnected;

        // TODO: ask from display driver
        disp_data->secure    = true;
        disp_data->hdcp_version = UINT_MAX;
        disp_data->subtype   = HWC_DISPLAY_LCM;

        disp_data->aspect_portrait  = float(info.displayWidth) / float(info.displayHeight);
        disp_data->aspect_landscape = float(info.displayHeight) / float(info.displayWidth);
        disp_data->mir_portrait     = Rect(info.displayWidth, info.displayHeight);
        disp_data->mir_landscape    = Rect(info.displayWidth, info.displayHeight);

        disp_data->vsync_source = HWC_DISPLAY_PRIMARY;

        int32_t refresh_rate = ovlDevice->getRefresh(dpy, 0);
        if (0 >= refresh_rate) refresh_rate = 60;
        disp_data->refresh = nsecs_t(1e9 / float(refresh_rate) + 0.5f);

        // get physically installed rotation for primary display from property
        property_get("ro.vendor.sf.hwrotation", value, "0");
        disp_data->hwrotation = static_cast<uint32_t>(atoi(value) / 90);

        disp_data->pixels = static_cast<uint32_t>(disp_data->width * disp_data->height);

        disp_data->trigger_by_vsync = false;

        disp_data->group = 0;

        // (w, h) key same = same group
        std::vector<std::pair<int, int>> group_list;
        group_list.emplace_back(disp_data->width, disp_data->height);

        // fill primary display extra config
        uint32_t nNumConfigs = ovlDevice->getNumConfigs(dpy);
        if (nNumConfigs > 1)
        {
            free(m_data_primary_extra);
            m_data_primary_extra = (DisplayData*)malloc((nNumConfigs - 1) * sizeof(DisplayData));
            LOG_ALWAYS_FATAL_IF(m_data_primary_extra == nullptr,
                "DisplayManager::m_data_primary_extra malloc(%zu) fail",
                (nNumConfigs - 1) * sizeof(DisplayData));

            for (unsigned int i = 0, config = 1; config < nNumConfigs; ++i, ++config)
            {
                memcpy(&m_data_primary_extra[i], disp_data, sizeof(DisplayData));
                m_data_primary_extra[i].width = ovlDevice->getWidth(dpy, config);
                m_data_primary_extra[i].height = ovlDevice->getHeight(dpy, config);
                m_data_primary_extra[i].xdpi = physicalWidth == 0 ? 0 :
                    m_data_primary_extra[i].width * 25.4f * 1000000 / physicalWidth;
                m_data_primary_extra[i].ydpi = physicalHeight == 0 ? 0 :
                    m_data_primary_extra[i].height * 25.4f * 1000000 / physicalHeight;
                m_data_primary_extra[i].aspect_portrait =
                    float(m_data_primary_extra[i].width) / float(m_data_primary_extra[i].height);
                m_data_primary_extra[i].aspect_landscape =
                    float(m_data_primary_extra[i].height) / float(m_data_primary_extra[i].width);
                m_data_primary_extra[i].mir_portrait =
                    Rect(m_data_primary_extra[i].width, m_data_primary_extra[i].height);
                m_data_primary_extra[i].mir_landscape =
                    Rect(m_data_primary_extra[i].width, m_data_primary_extra[i].height);
                refresh_rate = ovlDevice->getRefresh(dpy, config);
                m_data_primary_extra[i].refresh = nsecs_t(1e9 / float(refresh_rate) + 0.5f);
                m_data_primary_extra[i].pixels =
                    static_cast<uint32_t>(m_data_primary_extra[i].width * m_data_primary_extra[i].height);

                // group
                auto compare = [w = m_data_primary_extra[i].width, h = m_data_primary_extra[i].height](std::pair<int, int> key)
                               {
                                   return w == key.first && h == key.second;
                               };
                auto it = std::find_if(group_list.begin(), group_list.end(), compare);

                int group;
                if (it == group_list.end())
                {
                    group_list.emplace_back(m_data_primary_extra[i].width, m_data_primary_extra[i].height);
                    group = static_cast<int>(group_list.size() - 1);
                }
                else
                {
                    group = static_cast<int>(it - group_list.begin());
                }
                m_data_primary_extra[i].group = group;
            }
        }
    }
    else if (dpy == HWC_DISPLAY_EXTERNAL)
    {
        SessionInfo info;
        sp<IOverlayDevice> ovlDevice = HWCMediator::getInstance().getOvlDevice(dpy);
        ovlDevice->getOverlaySessionInfo(HWC_DISPLAY_EXTERNAL, &info);
        if (!info.isConnected)
        {
            HWC_LOGE("Failed to add display, hdmi is not connected!");
            return;
        }

        disp_data->width     = static_cast<int>(info.displayWidth);
        disp_data->height    = static_cast<int>(info.displayHeight);
        disp_data->format    = info.displayFormat;
        disp_data->density   = info.density == 0 ? static_cast<int>(density) : static_cast<int>(info.density);
        disp_data->xdpi      = info.physicalWidth == 0 ? disp_data->density :
                                (info.displayWidth * 25.4f / info.physicalWidth);
        disp_data->ydpi      = info.physicalHeight == 0 ? disp_data->density :
                                (info.displayHeight * 25.4f / info.physicalHeight);
        disp_data->has_vsync = info.isHwVsyncAvailable;
        disp_data->connected = info.isConnected;
        // TODO: check if hdcp 1.x is used

        disp_data->secure    = true;

        disp_data->hdcp_version = info.isHDCPSupported;
        HWC_LOGI("(%" PRIu64 ") hdcp version is %d", dpy, disp_data->hdcp_version);
        disp_data->subtype = HWC_DISPLAY_HDMI_MHL;

        // [NOTE]
        // only if the display without any physical rotation,
        // same ratio can be applied to both portrait and landscape
        disp_data->aspect_portrait  = float(info.displayWidth) / float(info.displayHeight);
        disp_data->aspect_landscape = disp_data->aspect_portrait;

        disp_data->vsync_source = HWC_DISPLAY_EXTERNAL;

        int32_t refresh_rate = ovlDevice->getCurrentRefresh(dpy);
        if (0 >= refresh_rate) refresh_rate = 60;
        disp_data->refresh = nsecs_t(1e9 / float(refresh_rate) + 0.5f);

        // get physically installed rotation for extension display from property
        property_get("ro.vendor.sf.hwrotation.ext", value, "0");
        disp_data->hwrotation = static_cast<uint32_t>(atoi(value) / 90);

        disp_data->pixels = static_cast<uint32_t>(disp_data->width * disp_data->height);

        disp_data->trigger_by_vsync =
            HwcFeatureList::getInstance().getFeature().trigger_by_vsync > 0 ? true : false;
    }
    else if (dpy == HWC_DISPLAY_VIRTUAL)
    {
        HWC_LOGE("%s(), HWC2 should not call for virtual displays", __FUNCTION__);
    }
}

void DisplayManager::setMirrorRegion(uint64_t dpy)
{
    DisplayData* main_disp_data = &m_data[HWC_DISPLAY_PRIMARY];
    DisplayData* disp_data = &m_data[dpy];

    // calculate portrait region
    if (main_disp_data->aspect_portrait > disp_data->aspect_portrait)
    {
        // calculate for letterbox
        int portrait_h = static_cast<int>(disp_data->width / main_disp_data->aspect_portrait);
        int portrait_y = static_cast<int>((disp_data->height - portrait_h) / 2);
        disp_data->mir_portrait.left = 0;
        disp_data->mir_portrait.top = portrait_y;
        disp_data->mir_portrait.right = disp_data->width;
        disp_data->mir_portrait.bottom = portrait_y + portrait_h;
    }
    else
    {
        // calculate for pillarbox
        int portrait_w = static_cast<int>(disp_data->height * main_disp_data->aspect_portrait);
        int portrait_x = static_cast<int>((disp_data->width - portrait_w) / 2);
        disp_data->mir_portrait.left = portrait_x;
        disp_data->mir_portrait.top = 0;
        disp_data->mir_portrait.right = portrait_x + portrait_w;
        disp_data->mir_portrait.bottom = disp_data->height;
    }

    // calculate landscape region
    if (main_disp_data->aspect_landscape > disp_data->aspect_landscape)
    {
        // calculate for letterbox
        int landscape_h = static_cast<int>(disp_data->width / main_disp_data->aspect_landscape);
        int landscape_y = static_cast<int>((disp_data->height - landscape_h) / 2);
        disp_data->mir_landscape.left = 0;
        disp_data->mir_landscape.top = landscape_y;
        disp_data->mir_landscape.right = disp_data->width;
        disp_data->mir_landscape.bottom = landscape_y + landscape_h;
    }
    else
    {
        // calculate for pillarbox
        int landscape_w = static_cast<int>(disp_data->height * main_disp_data->aspect_landscape);
        int landscape_x = static_cast<int>((disp_data->width - landscape_w) / 2);
        disp_data->mir_landscape.left = landscape_x;
        disp_data->mir_landscape.top = 0;
        disp_data->mir_landscape.right = landscape_x + landscape_w;
        disp_data->mir_landscape.bottom = disp_data->height;
    }
}

void DisplayManager::hotplugPost(uint64_t dpy, bool connected, int state, bool print_info)
{
    DisplayData* disp_data = &m_data[dpy];

    switch (state)
    {
        case DISP_PLUG_CONNECT:
            HWC_LOGI("Added Display Information:");
            setMirrorRegion(dpy);
            if (print_info)
            {
                printDisplayInfo(dpy);
            }
            compareDisplaySize(dpy);
            m_curr_disp_num++;
            break;

        case DISP_PLUG_DISCONNECT:
            HWC_LOGI("Removed Display Information:");
            if (print_info)
            {
                printDisplayInfo(dpy);
            }
            memset((void*)disp_data, 0, sizeof(DisplayData));
            compareDisplaySize(dpy);
            m_curr_disp_num--;
            break;

        case DISP_PLUG_NONE:
            HWC_LOGW("Unexpected hotplug: disp(%" PRIu64 ":%d) connect(%d)",
                     dpy, disp_data->connected, connected);
            return;
    };
}

void DisplayManager::setPowerMode(uint64_t dpy, int mode)
{
    Mutex::Autolock _l(m_power_lock);

    if (HWC_DISPLAY_PRIMARY != dpy) return;

    if (m_data[HWC_DISPLAY_EXTERNAL].connected &&
        m_data[HWC_DISPLAY_EXTERNAL].subtype == HWC_DISPLAY_SMARTBOOK)
    {
        m_data[dpy].vsync_source = (HWC2_POWER_MODE_OFF == mode) ?
                                        HWC_DISPLAY_EXTERNAL : HWC_DISPLAY_PRIMARY;
    }

    if (Platform::getInstance().m_config.dynamic_switch_path && (HWC_DISPLAY_PRIMARY == dpy))
    {
        display_set_primary_power(mode);
    }
}

void DisplayManager::compareDisplaySize(uint64_t dpy)
{
    if (dpy >= MAX_DISPLAYS) return;

    DisplayData* display_data = &m_data[dpy];
    AutoMutex _l(m_state_lock);
    if (display_data->connected)
    {
        // threshold should be the resolution of primary display
        // however, we only care MHL 4k output now.
        size_t size = static_cast<size_t>(display_data->width * display_data->height);
        m_state_ultra_display[dpy] = (size >= Platform::getInstance().getLimitedExternalDisplaySize());
    }
    else
    {
        m_state_ultra_display[dpy] = false;
    }
}

bool DisplayManager::isUltraDisplay(uint64_t dpy)
{
    AutoMutex _l(m_state_lock);
    return m_state_ultra_display[dpy];
}

bool DisplayManager::isAllDisplayOff()
{
    AutoMutex _l(m_state_lock);
    int res = 0;
    for (int i = 0; i < MAX_DISPLAYS; i++)
    {
        if (m_display_power_state[i])
        {
            res |= 1 << i;
        }
    }

    HWC_LOGD("all panel state: %x", res);
    return res ? false : true;
}

void DisplayManager::setDisplayPowerState(uint64_t dpy, int state)
{
    AutoMutex _l(m_state_lock);

    if (state == HWC2_POWER_MODE_OFF)
    {
        m_display_power_state[dpy] = false;
    }
    else
    {
        m_display_power_state[dpy] = true;
    }
}

int DisplayManager::getDisplayPowerState(const uint64_t& dpy)
{
    AutoMutex _l(m_state_lock);
    return m_display_power_state[dpy];
}

void DisplayManager::getPhysicalPanelSize(unsigned int *width, unsigned int *height,
                                          SessionInfo &info)
{
    if (width != NULL)
    {
        if (info.physicalWidthUm != 0)
        {
            *width = info.physicalWidthUm;
        }
        else
        {
            *width = info.physicalWidth * 1000;
        }
    }

    if (height != NULL)
    {
        if (info.physicalHeightUm != 0)
        {
            *height = info.physicalHeightUm;
        }
        else
        {
            *height = info.physicalHeight * 1000;
        }
    }
}

uint32_t DisplayManager::getVideoHdcp() const
{
    RWLock::AutoRLock _l(m_video_hdcp_rwlock);
    return m_video_hdcp;
}

void DisplayManager::setVideoHdcp(const uint32_t& video_hdcp)
{
    RWLock::AutoWLock _l(m_video_hdcp_rwlock);
    m_video_hdcp = video_hdcp;
}

int32_t DisplayManager::getSupportedColorMode(uint64_t dpy)
{
    return HWCMediator::getInstance().getOvlDevice(dpy)->getSupportedColorMode();
}

const DisplayData* DisplayManager::getDisplayData(uint64_t dpy, hwc2_config_t config)
{
    if (dpy != HWC_DISPLAY_PRIMARY || config == 0)
    {
        return &m_data[dpy];
    }
    else if (config < HWCMediator::getInstance().getOvlDevice(dpy)->getNumConfigs(dpy) &&
             m_data_primary_extra != nullptr)
    {
        return &m_data_primary_extra[config - 1];
    }
    HWC_LOGE("Fail to get DisplayData, dpy(%" PRIu64 ") config(%d/%d)", dpy, config,
        HWCMediator::getInstance().getOvlDevice(dpy)->getNumConfigs(dpy));
    return nullptr;
}

unsigned int DisplayManager::getNumberPluginDisplay()
{
    return m_curr_disp_num;
}

void DisplayManager::updateVsyncThreadPeriod(uint64_t dpy, nsecs_t period){
    AutoMutex _l(m_vsyncs[dpy].lock);
    if (m_vsyncs[dpy].thread != NULL)
    {
        m_vsyncs[dpy].thread->updatePeriod(period);
    }
}
