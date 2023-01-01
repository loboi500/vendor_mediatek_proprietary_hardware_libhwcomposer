#define DEBUG_LOG_TAG "DRMDEV"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "drmmoderesource.h"

#include <cutils/properties.h>

#include <linux/mediatek_drm.h>

#include <cutils/log.h>
#include <utils/Trace.h>
#include <errno.h>
#include <drm/drm_fourcc.h>

#include "utils/debug.h"
#include "utils/tools.h"

#include "drmmodecrtc.h"
#include "drmmodeencoder.h"
#include "drmmodeconnector.h"
#include "drmmodeplane.h"
#include "drmmodeutils.h"

#ifdef USE_SWWATCHDOG
#include "utils/swwatchdog.h"
#endif

#define DRM_DISPLAY_PATH "/dev/dri/card0"

#define DRM_DIM_FAKE_GEM_HANDLE 0xff44696D //0xff'Dim'
#define DRM_DIM_BUF_LENGTH 4096

using namespace android;

DrmModeResource& DrmModeResource::getInstance()
{
    static DrmModeResource gInstance;
    return gInstance;
}

DrmModeResource::DrmModeResource()
    : m_fd(-1)
    , m_dim_fb_id(0)
    , m_max_support_width(0)
    , m_max_support_height(0)
{
    memset(m_dsi_switch_configs, 0, sizeof(m_dsi_switch_configs));

    init();
}

DrmModeResource::~DrmModeResource()
{
    if (m_crtc_list.size() > 0)
    {
        for (size_t i = 0; i < m_crtc_list.size(); i++)
        {
            delete m_crtc_list[i];
        }
        m_crtc_list.clear();
    }

    if (m_encoder_list.size() > 0)
    {
        for (size_t i = 0; i < m_encoder_list.size(); i++)
        {
            delete m_encoder_list[i];
        }
        m_encoder_list.clear();
    }

    if (m_connector_list.size() > 0)
    {
        for (size_t i = 0; i < m_connector_list.size(); i++)
        {
            delete m_connector_list[i];
        }
        m_connector_list.clear();
    }

    if (m_plane_list.size() > 0)
    {
        for (size_t i = 0; i < m_plane_list.size(); i++)
        {
            delete m_plane_list[i];
        }
        m_plane_list.clear();
    }

    if (m_fd >= 0)
    {
        close(m_fd);
    }
}

int DrmModeResource::getFd()
{
    return m_fd;
}

int DrmModeResource::init()
{
    int res = 0;
    uint32_t retry = 0;
    do
    {
        m_fd = open(DRM_DISPLAY_PATH, O_RDWR);
        if (m_fd < 0)
        {
            HWC_LOGE("failed to open drm device[%s]: %d", DRM_DISPLAY_PATH, m_fd);
            return m_fd;
        }

        int isMaster = 0;
        res = ::ioctl(m_fd, DRM_IOCTL_MTK_GET_MASTER_INFO, &isMaster);
        if (res)
        {
            HWC_LOGE("failed to get drm master info, err: %d", res);
            break;
        }

        if (isMaster != 0)
        {
            break;
        }
        else
        {
            close(m_fd);
            m_fd = -1;
            if ((++retry % 100) == 1)
            {
                HWC_LOGE("failed to get drm master, retry: %u", retry);
            }
            usleep(0);
        }
    } while(true);

    queryCapsInfo();

    res = initDrmCap();
    if (res)
    {
        HWC_LOGE("failed to initialize drm cap");
        return res;
    }

    res = initDrmResource();
    if (res)
    {
        HWC_LOGE("failed to initialize drm resource");
    }

    res = queryPanelsInfoAndDispatch();
    if (res)
    {
        HWC_LOGE("failed to query panel info and dispatch");
    }

    arrangeResource();
    initDimFbId();

    return res;
}

void DrmModeResource::queryCapsInfo()
{
    ATRACE_NAME("DRM_IOCTL_MTK_GET_DISPLAY_CAPS");
    memset(&m_caps_info, 0, sizeof(m_caps_info));
    int err = ioctl(DRM_IOCTL_MTK_GET_DISPLAY_CAPS, &m_caps_info);
    if (err)
    {
        LOG_FATAL("DRM_IOCTL_MTK_GET_DISPLAY_CAPS fail err:%d", err);
    }
}

int DrmModeResource::initDrmCap()
{
    int res = drmSetClientCap(m_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (res)
    {
        HWC_LOGW("failed to set cap universal plane: %d", res);
        return res;
    }

    res = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (res)
    {
        HWC_LOGW("failed to set cap atomic: %d", res);
        return res;
    }

    res = drmSetClientCap(m_fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
    if (res)
    {
        HWC_LOGW("failed to set cap writeback connector: %d", res);
        // some kernel version does not support this capability, so print a warning message only
        res = 0;
    }

    return res;
}

int DrmModeResource::initDrmResource()
{
    drmModeResPtr res = drmModeGetResources(m_fd);

    if (!res)
    {
        HWC_LOGW("failed to get drm resource");
        return -ENODEV;
    }

    m_max_support_width = res->max_width;
    m_max_support_height = res->max_height;

    int last_error = 0;
    int ret = initDrmCrtc(res);
    if (ret)
    {
        HWC_LOGW("failed to initialize all crtc: %d", ret);
        last_error = ret;
    }

    ret = initDrmEncoder(res);
    if (ret)
    {
        HWC_LOGW("failed to initialize all encoder: %d", ret);
        last_error = ret;
    }

    ret = initDrmConnector(res);
    if (ret)
    {
        HWC_LOGW("failed to initialize all connector: %d", ret);
        last_error = ret;
    }

    ret = initDrmPlane();
    if (ret)
    {
        HWC_LOGW("failed to initialize all plane: %d", ret);
        last_error = ret;
    }

    //TODO remove drm resource in here now ?
    drmModeFreeResources(res);

    return last_error;
}

int DrmModeResource::initDrmCrtc(drmModeResPtr r)
{
    int res = 0;
    if (r->count_crtcs >= 0)
    {
        for (unsigned int i = 0; i < static_cast<unsigned int>(r->count_crtcs); i++)
        {
            drmModeCrtcPtr c = drmModeGetCrtc(m_fd, r->crtcs[i]);
            if (!c)
            {
                HWC_LOGW("failed to get crtc[%u]: %d", i, r->crtcs[i]);
                drmModeFreeCrtc(c);
                res = -ENODEV;
                break;
            }

            DrmModeCrtc *crtc = new DrmModeCrtc(this, c);
            drmModeFreeCrtc(c);
            if (crtc->init(m_fd, i))
            {
                HWC_LOGW("failed to initialize crtc[%u]: %d", i, r->crtcs[i]);
                res = -ENODEV;
            }
            m_crtc_list.push_back(crtc);

            if (i == 0)
            {
                crtc->setMainCrtc();
                if (m_caps_info.disp_feature_flag & DRM_DISP_FEATURE_MML_PRIMARY)
                {
                    crtc->setSupportMml();
                }
                if (m_caps_info.disp_feature_flag & DRM_DISP_FEATURE_RPO)
                {
                    crtc->setSupportRpo();
                }
            }
        }
    }
    return res;
}

int DrmModeResource::initDrmEncoder(drmModeResPtr r)
{
    int res = 0;
    for (int i = 0; i < r->count_encoders; i++)
    {
        drmModeEncoderPtr e = drmModeGetEncoder(m_fd, r->encoders[i]);
        if (!e)
        {
            HWC_LOGW("failed to get encoder[%d]: %d", i, r->encoders[i]);
            drmModeFreeEncoder(e);
            res = -ENODEV;
            break;
        }

        DrmModeEncoder *encoder = new DrmModeEncoder(e);
        drmModeFreeEncoder(e);
        if (encoder->init(m_fd))
        {
            HWC_LOGW("failed to initialize encoder[%d]: %d", i, r->encoders[i]);
            res = -ENODEV;
        }
        m_encoder_list.push_back(encoder);
    }
    return res;
}

int DrmModeResource::initDrmConnector(drmModeResPtr r)
{
    int res = 0;
    for (int i = 0; i < r->count_connectors; i++)
    {
        drmModeConnectorPtr c = drmModeGetConnector(m_fd, r->connectors[i]);
        if (!c)
        {
            HWC_LOGW("failed to get connector[%d]: %d", i, r->connectors[i]);
            drmModeFreeConnector(c);
            res = -ENODEV;
            break;
        }

        DrmModeConnector *connector = new DrmModeConnector(c);
        drmModeFreeConnector(c);
        if (connector->init(m_fd))
        {
            HWC_LOGW("failed to initialize connector[%d]: %d", i, r->connectors[i]);
            res = -ENODEV;
        }
        m_connector_list.push_back(connector);
    }
    return res;
}

int DrmModeResource::initDrmPlane()
{
    drmModePlaneResPtr r = drmModeGetPlaneResources(m_fd);
    if (!r)
    {
        HWC_LOGW("failed to get drm resource");
        return -ENODEV;
    }

    int res = 0;
    for (uint32_t i = 0; i < r->count_planes; i++)
    {
        drmModePlanePtr p = drmModeGetPlane(m_fd, r->planes[i]);
        if (!p)
        {
            HWC_LOGW("failed to get plane[%u]: %d", i, r->planes[i]);
            drmModeFreePlane(p);
            res = -ENODEV;
            break;
        }

        DrmModePlane *plane = new DrmModePlane(p);
        drmModeFreePlane(p);
        if (plane->init(m_fd))
        {
            HWC_LOGW("failed to initialize plane[%u]: %d", i, r->planes[i]);
            res = -ENODEV;
        }
        m_plane_list.push_back(plane);
    }
    drmModeFreePlaneResources(r);
    return res;
}

void DrmModeResource::arrangeResource()
{
    for (size_t i = 0; i < m_encoder_list.size(); i++)
    {
        m_encoder_list[i]->arrangeCrtc(m_crtc_list);
    }

    for (size_t i = 0; i < m_connector_list.size(); i++)
    {
        m_connector_list[i]->arrangeEncoder(m_encoder_list);
    }

    for (size_t i = 0; i < m_plane_list.size(); i++)
    {
        m_plane_list[i]->arrangeCrtc(m_crtc_list);
    }
}

void DrmModeResource::dumpResourceInfo()
{
    HWC_LOGI("List of all crtc (total:%zu)", m_crtc_list.size());
    for (size_t i = 0; i < m_crtc_list.size(); i++)
    {
        m_crtc_list[i]->dump();
    }

    HWC_LOGI("List of all encoder (total:%zu)", m_encoder_list.size());
    for (size_t i = 0; i < m_encoder_list.size(); i++)
    {
        m_encoder_list[i]->dump();
    }

    HWC_LOGI("List of all connector (total:%zu)", m_connector_list.size());
    for (size_t i = 0; i < m_connector_list.size(); i++)
    {
        m_connector_list[i]->dump();
    }

    HWC_LOGI("List of all plane (total:%zu)", m_plane_list.size());
    for (size_t i = 0; i < m_plane_list.size(); i++)
    {
        m_plane_list[i]->dump();
    }
}

void DrmModeResource::connectAllDisplay()
{
    // encoder may have multiple possible crtc, record if it's already paired
    std::vector<bool> already_paired_crtc(m_crtc_list.size(), false);

    // default connector/crtc pair
    std::vector<std::pair<DrmModeConnector*, DrmModeCrtc*>> dsi_switch_infos;

    for (DrmModeConnector* connector : m_connector_list)
    {
        if (!connector)
        {
            HWC_LOGE("nullptr in m_connector_list");
            HWC_ASSERT(0);
            continue;
        }

        // set encoder for this connector
        DrmModeEncoder* paired_encoder = nullptr;
        for (DrmModeEncoder* encoder : m_encoder_list)
        {
            if (!connector->connectEncoder(encoder))
            {
                paired_encoder = encoder;
                break;
            }
        }
        if (!paired_encoder)
        {
            HWC_LOGW("no encoder for connector id: %u", connector->getId());
            continue;
        }

        // set crtc for this encoder
        DrmModeCrtc *paired_crtc = nullptr;
        for (size_t i = 0; i < m_crtc_list.size(); i++)
        {
            if (already_paired_crtc[i])
            {
                continue;
            }

            if (!paired_encoder->connectCrtc(m_crtc_list[i]))
            {
                paired_crtc = m_crtc_list[i];
                already_paired_crtc[i] = true;
                break;
            }
        }
        if (!paired_crtc)
        {
            HWC_LOGW("no crtc for connector id: %u, encoder id: %u",
                     connector->getId(), paired_encoder->getId());
            continue;
        }

        DrmModeInfo mode;
        if (connector->getMode(&mode))
        {
            paired_crtc->setMode(mode);
        }

        // connect plane and crtc
        for (DrmModePlane* plane : m_plane_list)
        {
            plane->connectCrtc(paired_crtc);
        }
        if (paired_crtc->getPlaneNum() == 0)
        {
            HWC_LOGW("no plane for this crtc id: %u", paired_crtc->getId());
            continue;
        }

        // add this complete display into create display infos
        CreateDisplayInfo info;
        info.is_main = paired_crtc->isMainCrtc();
        info.drm_id_crtc_default = paired_crtc->getId();
        info.drm_id_connector = connector->getId();
        info.drm_id_crtc_alternative = UINT32_MAX;

        uint32_t type = connector->getConnectorType();
        bool valid_path = true;
        switch (type)
        {
            case DRM_MODE_CONNECTOR_DSI:
                info.is_internal = true;
                info.disp_type = HWC2_DISPLAY_TYPE_PHYSICAL;

                if (m_dsi_switch_func)
                {
                    dsi_switch_infos.push_back(std::make_pair(connector, paired_crtc));

                    for (size_t i = 0; i < m_crtc_list.size(); i++)
                    {
                        if ((paired_encoder->getPossibleCrtc() & (1 << m_crtc_list[i]->getPipe())) &&
                            m_crtc_list[i] != paired_crtc)
                        {
                            info.drm_id_crtc_alternative = m_crtc_list[i]->getId();
                            HWC_LOGI("%s(), connector %u alternative crtc %u",
                                     __FUNCTION__, connector->getId(), info.drm_id_crtc_alternative);
                            break;
                        }
                    }
                }
                break;
            case DRM_MODE_CONNECTOR_WRITEBACK:
                info.is_internal = false;
                info.disp_type = HWC2_DISPLAY_TYPE_VIRTUAL;
                break;
            default:
                if (connector->isConnectorTypeExternal())
                {
                    info.is_internal = false;
                    info.disp_type = HWC2_DISPLAY_TYPE_PHYSICAL;
                    break;
                }

                HWC_LOGE("%s(), unknow type %u", __FUNCTION__, type);
                valid_path = false;
                HWC_ASSERT(0);
                break;
        }
        if (valid_path)
        {
            m_create_display_infos.push_back(info);
        }
    }

    // error check
    bool has_internal = false;
    for (const CreateDisplayInfo& info : m_create_display_infos)
    {
        if (info.is_internal)
        {
            has_internal = true;
            break;
        }
    }
    if (!has_internal)
    {
        HWC_LOGW("%s(), no internal display", __FUNCTION__);
        HWC_ASSERT(0);
    }

    // init dsi switch
    if (m_dsi_switch_func)
    {
        if (dsi_switch_infos.size() >= 2)
        {
            // verify possible crtc
            uint32_t possible_crtcs = 0;
            possible_crtcs |= 1 << dsi_switch_infos[0].second->getPipe();
            possible_crtcs |= 1 << dsi_switch_infos[1].second->getPipe();

            DrmModeEncoder* encoder = dsi_switch_infos[0].second->getEncoder();
            if (encoder)
            {
                if (encoder->getPossibleCrtc() != possible_crtcs)
                {
                    HWC_LOGE("%s(), possible_crtcs 0x%x != encoder[0]->getPossibleCrtc() 0x%x",
                             __FUNCTION__, possible_crtcs, encoder->getPossibleCrtc());
                    HWC_ASSERT(0);
                    m_dsi_switch_func = false;
                }
            }
            encoder = dsi_switch_infos[1].second->getEncoder();
            if (encoder)
            {
                if (encoder->getPossibleCrtc() != possible_crtcs)
                {
                    HWC_LOGE("%s(), possible_crtcs 0x%x != encoder[1]->getPossibleCrtc() 0x%x",
                             __FUNCTION__, possible_crtcs, encoder->getPossibleCrtc());
                    HWC_ASSERT(0);
                    m_dsi_switch_func = false;
                }
            }

            // setup dsi switch configs
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_DISABLE].connector_1 = dsi_switch_infos[0].first;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_DISABLE].crtc_1 = dsi_switch_infos[0].second;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_DISABLE].connector_2 = dsi_switch_infos[1].first;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_DISABLE].crtc_2 = dsi_switch_infos[1].second;

            m_dsi_switch_configs[DSI_SWITCH_CONFIG_ENABLE].connector_1 = dsi_switch_infos[0].first;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_ENABLE].crtc_1 = dsi_switch_infos[1].second;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_ENABLE].connector_2 = dsi_switch_infos[1].first;
            m_dsi_switch_configs[DSI_SWITCH_CONFIG_ENABLE].crtc_2 = dsi_switch_infos[0].second;

            HWC_LOGW("%s(), m_dsi_switch_configs config succeed", __FUNCTION__);
        }
        else
        {
            HWC_LOGW("%s(), dsi_switch_infos %zu not enough", __FUNCTION__, dsi_switch_infos.size());
            HWC_ASSERT(0);
            m_dsi_switch_func = false;
        }

        // init dsi switch false
        if (m_dsi_switch_func)
        {
            setDsiSwitchEnable(false, true);
        }
    }
}

void DrmModeResource::getCreateDisplayInfos(std::vector<CreateDisplayInfo>& create_display_infos)
{
    create_display_infos = m_create_display_infos;
}

int DrmModeResource::setDisplay(uint64_t dpy, uint32_t id_crtc, bool use_req_size)
{
    DrmModeCrtc *crtc = getCrtc(id_crtc);
    if (crtc == nullptr)
    {
        HWC_LOGW("failed to set display_%" PRIu64 ": no crtc", dpy);
        return -ENODEV;
    }

    DrmModeEncoder *encoder = crtc->getEncoder();
    if (encoder == nullptr)
    {
        HWC_LOGW("failed to set display_%" PRIu64 ": no encoder", dpy);
        return -ENODEV;
    }

    DrmModeConnector *connector = encoder->getConnector();
    if (connector == nullptr)
    {
        HWC_LOGW("failed to set display_%" PRIu64 ": no connector", dpy);
        return -ENODEV;
    }
    struct hwc_drm_bo prev_fb_bo = crtc->getDumbBuffer();
    int res = crtc->prepareFb();
    if (res != 0) {
        HWC_LOGW("failed to set display_%" PRIu64 ": prepare buffer", dpy);
        return res;
    }

    uint32_t c_id = connector->getId();
    DrmModeInfo mode;
    connector->getMode(&mode);
    drmModeModeInfo modeInfo;
    if (use_req_size)
    {
        uint16_t w = static_cast<uint16_t>(crtc->getReqWidth());
        uint16_t h = static_cast<uint16_t>(crtc->getReqHeight());
        modeInfo.hdisplay = w;
        modeInfo.hsync_start = w;
        modeInfo.hsync_end = w;
        modeInfo.htotal = w;
        modeInfo.hskew = 0;
        modeInfo.vdisplay = h;
        modeInfo.vsync_start = h;
        modeInfo.vsync_end = h;
        modeInfo.vtotal = h;
        modeInfo.vscan = 0;
        modeInfo.vrefresh = 60;
        modeInfo.flags = 0;
        modeInfo.type = 0;
        modeInfo.clock = w * h * modeInfo.vrefresh / 1000;
        if (modeInfo.clock < 1)
        {
            modeInfo.clock = 1;
        }
    }
    else if(dpy == HWC_DISPLAY_EXTERNAL)
    {
        connector->getMode(&mode, 0, true);
        mode.getModeInfo(&modeInfo);
    }
    else
    {
        connector->getMode(&mode);
        mode.getModeInfo(&modeInfo);
    }

    {
        ATRACE_NAME("SetCrtc");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(SetCrtc):" STRINGIZE(__LINE__), 500);
#endif
        res = drmModeSetCrtc(m_fd, crtc->getId(), crtc->getFbId(), 0, 0, &c_id, 1, &modeInfo);
    }
    if (res)
    {
        HWC_LOGE("drmModeSetCrtc fail ret:%d %s", res, strerror(errno));
    }
    if (prev_fb_bo.fb_id != 0)
    {
        int result = crtc->destroyFb(prev_fb_bo);
        if (result != 0)
        {
            HWC_LOGE("%s destroyFb fail err:%d", __func__ , result);
        }
    }
    return res;
}

int DrmModeResource::blankDisplay(uint64_t dpy, uint32_t id_crtc, int mode, bool panel_stay_on)
{
    DrmModeCrtc *crtc = getCrtc(id_crtc);
    if (!crtc)
    {
        HWC_LOGE("(%" PRIu64 ":%u) %s: drm state is wrong", dpy, id_crtc, __func__);
        return -ENODEV;
    }
    DrmModeEncoder *encoder = crtc->getEncoder();
    if (encoder == nullptr)
    {
        HWC_LOGE("(%" PRIu64 ":%u) %s: no encoder", dpy, id_crtc, __func__);
        return -ENODEV;
    }

    DrmModeConnector *connector = encoder->getConnector();
    if (connector == nullptr)
    {
        HWC_LOGE("(%" PRIu64 ":%u) %s: no connector", dpy, id_crtc, __func__);
        return -ENODEV;
    }

    drmModeAtomicReqPtr atomic_req;
    atomic_req = drmModeAtomicAlloc();
    if (atomic_req == nullptr)
    {
        HWC_LOGE("(%" PRIu64 ":%u) %s: failed to allocate atomic requirement", dpy, id_crtc, __func__);
        return -ENOMEM;
    }

    int res = 0;

    if (mode == HWC_POWER_MODE_DOZE || mode == HWC_POWER_MODE_NORMAL)
    {
        res |= crtc->addProperty(atomic_req, DRM_PROP_CRTC_ACTIVE, true) < 0;
    }
    else
    {
        res |= crtc->addProperty(atomic_req, DRM_PROP_CRTC_ACTIVE, false) < 0;
    }
    if (mode == HWC_POWER_MODE_DOZE || mode == HWC_POWER_MODE_DOZE_SUSPEND)
    {
        res |= crtc->addProperty(atomic_req, DRM_PROP_CRTC_DOZE_ACTIVE, true) < 0;
    }
    else
    {
        res |= crtc->addProperty(atomic_req, DRM_PROP_CRTC_DOZE_ACTIVE, false) < 0;
    }

    // when power off for dsi switch, set hint to driver not to close panel
    if (mode == HWC_POWER_MODE_OFF && panel_stay_on)
    {
        res |= crtc->addProperty(atomic_req, DRM_PROP_CRTC_USER_SCEN, 1  << 1) < 0;
    }

    //Virtual's HWC_POWER_MODE_OFF will be called by destroySession, it need to set plane off
    //or it will make DRM keep buffer then cause iova leak
    //And HWC_DISPLAY_PRIMARY's HWC_POWER_MODE_DOZE_SUSPEND has same problem too, because we
    //didn't disable any plane in HWC_POWER_MODE_DOZE_SUSPEND, it need to be handled.
    if ((connector->getConnectorType() == DRM_MODE_CONNECTOR_WRITEBACK && mode == HWC_POWER_MODE_OFF) ||
        (connector->getConnectorType() == DRM_MODE_CONNECTOR_DSI && mode == HWC_POWER_MODE_DOZE_SUSPEND))
    {
        for (size_t i = 0; i < crtc->getPlaneNum(); i++)
        {
            auto plane = crtc->getPlane(i);
            if (CC_UNLIKELY(!plane)) {
                HWC_LOGE("%s(), (%" PRIu64 ":%u) plane[%zu] == null",
                         __FUNCTION__, dpy, id_crtc, i);
                continue;
            }

            status_t ret = NO_ERROR;
            ret |= plane->addProperty(atomic_req, DRM_PROP_PLANE_CRTC_ID, 0) < 0;
            ret |= plane->addProperty(atomic_req, DRM_PROP_PLANE_FB_ID, 0) < 0;
            if (ret)
            {
                HWC_LOGE("%s(), (%" PRIu64 ":%u) failed to disable plane[%zu]",
                         __FUNCTION__, dpy, id_crtc, i);
            }
        }
    }

    res = atomicCommit(atomic_req, DRM_MODE_ATOMIC_ALLOW_MODESET, this);
    if (res)
    {
        HWC_LOGE("(%" PRIu64 ":%u) %s: failed to do atomic commit ret=%d", dpy, id_crtc, __func__, res);
    }
    else
    {
        HWC_LOGD("(%" PRIu64 ":%u) %s: drmModeAtomicCommit: %d", dpy, id_crtc, __func__, res);
    }
    drmModeAtomicFree(atomic_req);
    return res;
}

int DrmModeResource::addFb(struct hwc_drm_bo *bo)
{
    int res = 0;

    HWC_LOGV("add fb w:%u h:%u f:%x hnd:%u p:%u o:%u",
              bo->width, bo->height, bo->format, bo->gem_handles[0],
              bo->pitches[0], bo->offsets[0]);
    {
        ATRACE_NAME("AddFB2");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(AddFB2):" STRINGIZE(__LINE__), 500);
#endif
        res = drmModeAddFB2WithModifiers(m_fd, bo->width, bo->height, bo->format, bo->gem_handles,
                bo->pitches, bo->offsets, bo->modifier, &(bo->fb_id), DRM_MODE_FB_MODIFIERS);
    }
    if (res)
    {
        HWC_LOGE("failed to add fb ret=%d, w:%u h:%u f:%x hnd:%u p:%u o:%u",
                  res, bo->width, bo->height, bo->format, bo->gem_handles[0],
                  bo->pitches[0], bo->offsets[0]);
    }
    else
    {
        if (!isUserLoad())
        {
            std::lock_guard<std::mutex> lock(m_cur_fb_lock);
            m_cur_fb_ids.insert(bo->fb_id);
        }
    }

    return res;
}

int DrmModeResource::addFb(uint32_t gem_handle, unsigned int width, unsigned int height,
                           unsigned int stride, unsigned int format, int blending, bool secure,
                           uint32_t *fb_id)
{
    int res = 0;

    struct hwc_drm_bo bo;
    memset(&bo, 0, sizeof(struct hwc_drm_bo));

    bo.width = width;
    bo.height = height;
    bo.format = format;
    uint32_t num_plane = getPlaneNumberOfDispColorFormat(format);
    if (num_plane == 1)
    {
        bo.gem_handles[0] = gem_handle;
        bo.pitches[0] = stride * getDrmBitsPerPixel(format) / 8;
        bo.offsets[0] = 0;
    }
    else if (num_plane == 2 || num_plane == 3)
    {
        uint32_t offsets = 0;
        for (uint32_t i = 0; i < num_plane; i++)
        {
            uint32_t plane_size = 0;
            bo.gem_handles[i] = gem_handle;
            if (i == 0)
            {
                bo.pitches[i] = stride * getPlaneDepthOfDispColorFormat(format, i);
                plane_size = bo.pitches[i] * height;
            }
            else
            {
                bo.pitches[i] = (stride / getHorizontalSubSampleOfDispColorFormat(format)) * getPlaneDepthOfDispColorFormat(format, i);
                plane_size = bo.pitches[i] * height / getVerticalSubSampleOfDispColorFormat(format);
            }
            bo.offsets[i] = offsets;
            offsets += plane_size;
        }
    }

    // set modifer
    if (blending == HWC2_BLEND_MODE_PREMULTIPLIED)
    {
        bo.modifier[0] |= DRM_FORMAT_MOD_MTK_PREMULTIPLIED;
    }

    if (secure && isSupportDmaBuf())
    {
        bo.modifier[0] |= DRM_FORMAT_MOD_MTK_SECURE;
    }

    // all plane must have same modifer
    for (uint32_t i = 1; i < num_plane; i++)
    {
        bo.modifier[i] = bo.modifier[0];
    }

    res = addFb(&bo);
    if (!res)
    {
        *fb_id = bo.fb_id;
    }

    return res;
}

int DrmModeResource::removeFb(uint32_t fb_id)
{
    int res = 0;

    {
        ATRACE_NAME("RmFB");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(AtomicCommit):" STRINGIZE(__LINE__), 500);
#endif
        res = drmModeRmFB(m_fd, fb_id);
        if (!isUserLoad())
        {
            std::lock_guard<std::mutex> lock(m_cur_fb_lock);
            m_cur_fb_ids.erase(fb_id);
        }
    }
    if (res)
    {
        HWC_LOGE("failed to remove fb ret=%d, id:%u", res, fb_id);
    }

    return res;
}

int DrmModeResource::allocateBuffer(struct hwc_drm_bo *fb_bo)
{
    int res = 0;
    struct drm_mode_create_dumb create_arg;

    memset(&create_arg, 0, sizeof (create_arg));
    create_arg.bpp = getDrmBitsPerPixel(fb_bo->format);
    create_arg.width = fb_bo->width;
    create_arg.height = fb_bo->height;
    HWC_LOGD("allocateBuffer: %ux%u  format:0x%08x", fb_bo->width, fb_bo->height, fb_bo->format);
    {
        ATRACE_NAME("DRM_IOCTL_MODE_CREATE_DUMB");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(DRM_IOCTL_MODE_CREATE_DUMB):" STRINGIZE(__LINE__), 500);
#endif
        res = drmIoctl(DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
    }
    if (res)
    {
        HWC_LOGE("%s: failed to create dumb buffer: %s (wxh=%ux%u bpp=%u)",
                __func__, strerror(errno), create_arg.width, create_arg.height, create_arg.bpp);
        return res;
    }

    fb_bo->pitches[0] = create_arg.pitch;
    fb_bo->gem_handles[0] = create_arg.handle;
    fb_bo->offsets[0] = 0;
    res = addFb(fb_bo);
    if (res)
    {
        HWC_LOGW("%s: faile to add fb: %d", __func__, res);
        struct drm_mode_destroy_dumb destroy_arg;
        memset(&destroy_arg, 0, sizeof (destroy_arg));
        destroy_arg.handle = fb_bo->gem_handles[0];
        drmIoctl(DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
        return res;
    }

    return res;
}

int DrmModeResource::freeBuffer(struct hwc_drm_bo &fb_bo)
{
    int res = 0;
    struct drm_mode_destroy_dumb destroy_arg;
    memset(&destroy_arg, 0, sizeof (destroy_arg));
    destroy_arg.handle = fb_bo.gem_handles[0];
    res = removeFb(fb_bo.fb_id);
    if (res)
    {
        HWC_LOGW("%s: failed to remove fb: %d", __func__, res);
    }

    {
        ATRACE_NAME("DRM_IOCTL_MODE_DESTROY_DUMB");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(DRM_IOCTL_MODE_DESTROY_DUMB):" STRINGIZE(__LINE__), 500);
#endif
        res = drmIoctl(DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
    }
    return res;
}

int DrmModeResource::getHandleFromPrimeFd(int fd, uint32_t* gem_handle)
{
    int res = 0;

    {
        ATRACE_NAME("PrimeFDToHandle");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(PrimeFDToHandle):" STRINGIZE(__LINE__), 500);
#endif
        res = drmPrimeFDToHandle(m_fd, fd, gem_handle);
    }
    if (res)
    {
        HWC_LOGE("failed to import prime fd ret=%d, fd:%d", res, fd);
    }

    return res;
}

DrmModeCrtc* DrmModeResource::getCrtc(uint32_t id_crtc)
{
    for (DrmModeCrtc* crtc : m_crtc_list)
    {
        if (!crtc)
        {
            continue;
        }

        if (crtc->getId() == id_crtc)
        {
            return crtc;
        }
    }

    HWC_LOGE("%s(), can't find crtc %u", __FUNCTION__, id_crtc);
    abort();
    return nullptr;
}

DrmModeCrtc* DrmModeResource::getCrtcMain()
{
    for (DrmModeCrtc* crtc : m_crtc_list)
    {
        if (!crtc)
        {
            continue;
        }

        if (crtc->isMainCrtc())
        {
            return crtc;
        }
    }

    HWC_LOGE("%s(), no main crtc", __FUNCTION__);
    abort();
    return nullptr;
}

DrmModeConnector* DrmModeResource::getConnector(uint32_t id_connector)
{
    for (DrmModeConnector* connector : m_connector_list)
    {
        if (!connector)
        {
            continue;
        }

        if (connector->getId() == id_connector)
        {
            return connector;
        }
    }

    HWC_LOGE("%s(), can't find connector %u", __FUNCTION__, id_connector);
    abort();
    return nullptr;
}

int DrmModeResource::waitNextVsync(uint32_t id_crtc, nsecs_t* ts)
{
    DrmModeCrtc *crtc = getCrtc(id_crtc);
    if (crtc == nullptr)
    {
        HWC_LOGW("Failed to find crtc(%u) when %s", id_crtc, __FUNCTION__);
        return -ENODEV;
    }

    uint32_t high_crtc = (crtc->getPipe() << DRM_VBLANK_HIGH_CRTC_SHIFT);
    drmVBlank vblank;
    memset(&vblank, 0, sizeof(vblank));
    vblank.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | (high_crtc & DRM_VBLANK_HIGH_CRTC_MASK));
    vblank.request.sequence = 1;

    int ret = drmWaitVBlank(m_fd, &vblank);
    if (ret)
    {
        return ret;
    }
    *ts = static_cast<nsecs_t>(vblank.reply.tval_sec) * 1000000000 +
          static_cast<nsecs_t>(vblank.reply.tval_usec) * 1000;

    return 0;
}

int DrmModeResource::atomicCommit(drmModeAtomicReqPtr req, uint32_t flags, void *user_data)
{
    int res = 0;

    {
        ATRACE_NAME("AtomicCommit");
#ifdef USE_SWWATCHDOG
        SWWatchDog::AutoWDT _wdt("[DEV] ioctl(AtomicCommit):" STRINGIZE(__LINE__), 500);
#endif
        res = drmModeAtomicCommit(m_fd, req, flags, user_data);
    }

    return res;
}

int32_t DrmModeResource::getWidth(uint32_t id_connector, uint32_t config)
{
    DrmModeConnector *connector = getConnector(id_connector);
    if (connector != nullptr)
    {
        return static_cast<int32_t>(connector->getModeWidth(config));
    }
    return 0;
}

int32_t DrmModeResource::getHeight(uint32_t id_connector, uint32_t config)
{
    DrmModeConnector *connector = getConnector(id_connector);
    if (connector != nullptr)
    {
        return static_cast<int32_t>(connector->getModeHeight(config));
    }
    return 0;
}

int32_t DrmModeResource::getRefresh(uint32_t id_connector, uint32_t config)
{
    DrmModeConnector *connector = getConnector(id_connector);
    if (connector != nullptr)
    {
        return static_cast<int32_t>(connector->getModeRefresh(config));
    }
    return 0;
}

uint32_t DrmModeResource::getNumConfigs(uint32_t id_connector)
{
    DrmModeConnector *connector = getConnector(id_connector);
    if (connector != nullptr)
    {
        return static_cast<uint32_t>(connector->getModeNum());
    }
    return 0;
}

void DrmModeResource::initDimFbId()
{
    int res = addFb(DRM_DIM_FAKE_GEM_HANDLE, DRM_DIM_BUF_LENGTH, DRM_DIM_BUF_LENGTH,
            DRM_DIM_BUF_LENGTH, DRM_FORMAT_C8, HWC2_BLEND_MODE_PREMULTIPLIED, false, &m_dim_fb_id);
    if (res != 0)
    {
        HWC_LOGE("failed to create fb id of dim: %d", res);
    }
    else
    {
        HWC_LOGI("create dim info: dim_fb_id[%d]", m_dim_fb_id);
    }
}

int DrmModeResource::createPropertyBlob(const void *data, size_t length, uint32_t *id)
{
    if (data != nullptr && id != nullptr)
    {
        return drmModeCreatePropertyBlob(m_fd, data, length, id);
    }
    return -EINVAL;
}

int DrmModeResource::destroyPropertyBlob(uint32_t id)
{
    return drmModeDestroyPropertyBlob(m_fd, id);
}

uint32_t DrmModeResource::getMaxSupportWidth()
{
    return m_max_support_width;
}

uint32_t DrmModeResource::getMaxSupportHeight()
{
    return m_max_support_height;
}

unsigned int DrmModeResource::getMaxPlaneNum() const
{
    size_t max = 0;

    for (DrmModeCrtc* crtc : m_crtc_list)
    {
        size_t cur_plan_num = crtc->getPlaneNum();
        if (cur_plan_num > max)
        {
            max = cur_plan_num;
        }
    }

    return static_cast<unsigned int>(max);
}

int32_t DrmModeResource::updateCrtcToPreferredModeInfo()
{
    int fd = getFd();
    drmModeResPtr res = drmModeGetResources(fd);
    if (res == nullptr)
    {
        HWC_LOGE("%s display configuration information is nullptr", __func__);
        return NO_INIT;
    }

    // update connector/crtc mode for external display
    int32_t ret = NO_ERROR;
    for (DrmModeCrtc* crtc : m_crtc_list)
    {

        DrmModeEncoder* encoder = crtc->getEncoder();
        if (encoder == nullptr)
        {
            HWC_LOGE("%s encoder is nullptr", __func__);
            continue;
        }

        DrmModeConnector* connector = encoder->getConnector();
        if (connector == nullptr)
        {
            HWC_LOGE("%s connector is nullptr", __func__);
            continue;
        }

        if (!connector->isConnectorTypeExternal())
        {
            continue;
        }

        bool connector_updated = false;
        for (int i = 0; i < res->count_connectors; i++)
        {
            drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c)
            {
                HWC_LOGW("failed to get connector when %s [%d]: %d", __func__, i, res->connectors[i]);
                drmModeFreeConnector(c);
                continue;
            }
            HWC_LOGI("%s c_id:%d c_encoder_id:%d current_id:%d mode_count:%d",
                    __func__, c->connector_id, c->encoder_id, connector->getId(),c->count_modes);
            if (c->connector_id == connector->getId())
            {
                connector->setDrmModeInfo(c);
                connector_updated = true;
                drmModeFreeConnector(c);
                break;
            }
            else
            {
                drmModeFreeConnector(c);
            }
        }

        if (!connector_updated)
        {
            continue;
        }

        DrmModeInfo mode;
        if (connector->getMode(&mode, 0, true))
        {
            crtc->setMode(mode);
        }
        else
        {
            HWC_LOGE("%s: failed to get valid mode[crtc:%u connector:%u]", __func__, crtc->getId(),
                    connector->getId());
            ret = NAME_NOT_FOUND;
        }
    }

    drmModeFreeResources(res);

    return ret;
}

int32_t DrmModeResource::getCurrentRefresh(uint32_t id_crtc)
{
    DrmModeCrtc *crtc = getCrtc(id_crtc);
    if (crtc != nullptr)
    {
        return static_cast<int32_t>(crtc->getCurrentModeRefresh());
    }
    return 0;
}

const mtk_drm_disp_caps_info& DrmModeResource::getCapsInfo()
{
    return m_caps_info;
}

void DrmModeResource::dump(String8* str)
{
    if (CC_UNLIKELY(!str))
    {
        return;
    }

    if (isUserLoad())
    {
        str->appendFormat("Current fb in DrmModeResource\n");
        std::lock_guard<std::mutex> lock(m_cur_fb_lock);
        for (auto id : m_cur_fb_ids)
        {
            str->appendFormat("fb_id: %u\n", id);
        }
    }

   str->appendFormat("m_dsi_switch_func %d, m_dsi_switch %d\n", m_dsi_switch_func, m_dsi_switch);
}

void DrmModeResource::setDsiSwitchFunctionEnable(bool enable)
{
    m_dsi_switch_func = enable;
}

int DrmModeResource::setDsiSwitchEnable(bool enable, bool init)
{
    if (m_dsi_switch == enable && init == false)
    {
        return 0;
    }
    HWC_ATRACE_FORMAT_NAME("%s, %d -> %d", __FUNCTION__, m_dsi_switch, enable);

    m_dsi_switch = enable;

    drmModeAtomicReqPtr atomic_req;
    atomic_req = drmModeAtomicAlloc();
    if (atomic_req == nullptr)
    {
        HWC_LOGE("%s(), failed to allocate atomic requirement", __FUNCTION__);
        return -ENOMEM;
    }

    int ret = 0;

    DsiSwtichConfig& config = enable ?
                              m_dsi_switch_configs[DSI_SWITCH_CONFIG_ENABLE] :
                              m_dsi_switch_configs[DSI_SWITCH_CONFIG_DISABLE];

    // switch mode
    uint32_t crtc_1_new_selected_mode = config.crtc_2->getSelectedMode();
    uint32_t crtc_2_new_selected_mode = config.crtc_1->getSelectedMode();
    ret |= config.crtc_1->addProperty(atomic_req, DRM_PROP_CRTC_DISP_MODE_IDX, crtc_1_new_selected_mode);
    config.crtc_1->setSelectedMode(crtc_1_new_selected_mode);
    ret |= config.crtc_2->addProperty(atomic_req, DRM_PROP_CRTC_DISP_MODE_IDX, crtc_2_new_selected_mode);
    config.crtc_2->setSelectedMode(crtc_2_new_selected_mode);

    // switch crtc/connector connection
    ret |= config.connector_1->addProperty(atomic_req, DRM_PROP_CONNECTOR_CRTC_ID, config.crtc_1->getId());
    ret |= config.connector_2->addProperty(atomic_req, DRM_PROP_CONNECTOR_CRTC_ID, config.crtc_2->getId());

    HWC_LOGI("%s(), conn %u -> crtc id %u, conn %u -> crtc id %u",
             __FUNCTION__,
             config.connector_1->getId(), config.crtc_1->getId(),
             config.connector_2->getId(), config.crtc_2->getId());

    ret = atomicCommit(atomic_req, DRM_MODE_ATOMIC_ALLOW_MODESET, this);
    if (ret)
    {
        HWC_LOGE("%s(), failed to do atomic commit ret %d", __FUNCTION__, ret);
        if (!init)
        {
            HWC_ASSERT(0);
        }
    }
    else
    {
        HWC_LOGD("%s(), drmModeAtomicCommit: %d", __FUNCTION__, ret);
    }
    drmModeAtomicFree(atomic_req);
    return ret;
}

int DrmModeResource::queryPanelsInfoAndDispatch()
{
    ATRACE_NAME("DRM_IOCTL_MTK_GET_PANELS_INFO");
    mtk_drm_panels_info panels_info;
    memset(&panels_info, 0, sizeof(panels_info));

    // let kernel know that this is to query connector cnt
    panels_info.connector_cnt = -1;
    int err = ioctl(DRM_IOCTL_MTK_GET_PANELS_INFO, &panels_info);

    if (err)
    {
        HWC_LOGE("%s, DRM_IOCTL_MTK_GET_PANELS_INFO(first) fail err:%d", __FUNCTION__, err);
        return err;
    }
    HWC_LOGI("%s, connector_cnt:%d, default_connector_id:%d",
                __FUNCTION__, panels_info.connector_cnt, panels_info.default_connector_id);

    panels_info.connector_obj_id = new unsigned int[static_cast<uint32_t>(panels_info.connector_cnt)];
    if (panels_info.connector_obj_id == nullptr)
    {
        HWC_LOGE("%s, new connector_obj_id failed fail", __FUNCTION__);
        return -ENOMEM;
    }

    panels_info.panel_name = new char *[static_cast<uint32_t>(panels_info.connector_cnt)];
    if (panels_info.panel_name == nullptr)
    {
        HWC_LOGE("%s, new panel_name failed fail", __FUNCTION__);
        releasePanelsInfo(panels_info);
        return -ENOMEM;
    }
    for (int i = 0; i < panels_info.connector_cnt; i++)
    {
        panels_info.panel_name[i] = new char[GET_PANELS_STR_LEN];
        if (panels_info.panel_name[i] == nullptr) {
            HWC_LOGE("%s, new memory failed fail", __FUNCTION__);
            releasePanelsInfo(panels_info);
            return -ENOMEM;
        }
    }

    panels_info.panel_id = new unsigned int[static_cast<uint32_t>(panels_info.connector_cnt)];
    if (panels_info.panel_id == nullptr)
    {
        HWC_LOGE("%s, new panel_id failed fail", __FUNCTION__);
        releasePanelsInfo(panels_info);
        return -ENOMEM;
    }

    err = ioctl(DRM_IOCTL_MTK_GET_PANELS_INFO, &panels_info);
    if (err)
    {
        HWC_LOGE("%s, DRM_IOCTL_MTK_GET_PANELS_INFO(second) fail err:%d", __FUNCTION__, err);
    }

    for (int i = 0; i < panels_info.connector_cnt; i++)
    {
        HWC_LOGI("%s, panels_info.panel_name[%d]: %s", __func__, i, panels_info.panel_name[i]);
        HWC_LOGI("%s, panels_info.connector_obj_id[%d]: %d", __func__, i, panels_info.connector_obj_id[i]);
    }

    // set to connector
    for (DrmModeConnector* connector : m_connector_list)
    {
        if (!connector)
        {
            continue;
        }

        for (int i = 0; i < panels_info.connector_cnt; i++)
        {
            if (panels_info.connector_obj_id[i] == connector->getId())
            {
                connector->setPanelInfo(panels_info.panel_name[i], panels_info.panel_id[i]);
                break;
            }
        }
    }

    // release resource
    releasePanelsInfo(panels_info);

    return err;
}

void DrmModeResource::releasePanelsInfo(mtk_drm_panels_info& panels_info)
{
    if (panels_info.connector_obj_id)
    {
        delete[] panels_info.connector_obj_id;
    }

    if (panels_info.panel_name)
    {
        for (int i = 0; i < panels_info.connector_cnt; i++)
        {
            if (panels_info.panel_name[i])
            {
                delete panels_info.panel_name[i];
            }
        }
        delete[] panels_info.panel_name;
    }

    if (panels_info.panel_id)
    {
        delete[] panels_info.panel_id;
    }
}

