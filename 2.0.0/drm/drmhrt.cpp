#define DEBUG_LOG_TAG "HRT"

#include "drmhrt.h"

#include <vector>

#include "drm/drmmodeutils.h"
#include "overlay.h"
#include "sync.h"
#include "dispatcher.h"
#include "hwc2.h"
#include "dev_interface.h"
#include "drm/drmdev.h"
#include <drm_fourcc.h>
#include <hwc_feature_list.h>
#include "mtk-mml.h"

static int32_t mapMtkLayeringCaps2HwcLayeringCaps(unsigned int caps)
{
    int32_t tmp = 0;

    if (caps & MTK_LAYERING_OVL_ONLY)
    {
        tmp |= HWC_LAYERING_OVL_ONLY;
    }

    if (caps & MTK_MDP_RSZ_LAYER)
    {
        tmp |= HWC_MDP_RSZ_LAYER;
    }

    if (caps & MTK_DISP_RSZ_LAYER)
    {
        tmp |= HWC_DISP_RSZ_LAYER;
    }

    if (caps & MTK_MDP_ROT_LAYER)
    {
        tmp |= HWC_MDP_ROT_LAYER;
    }

    if (caps & MTK_MDP_HDR_LAYER)
    {
        tmp |= HWC_MDP_HDR_LAYER;
    }

    if (caps & MTK_NO_FBDC)
    {
        tmp |= HWC_NO_FBDC;
    }

    if (caps & MTK_CLIENT_CLEAR_LAYER)
    {
        tmp |= HWC_CLIENT_CLEAR_LAYER;
    }

    if (caps & MTK_DISP_CLIENT_CLEAR_LAYER)
    {
        tmp |= HWC_DISP_CLIENT_CLEAR_LAYER;
    }

    if (caps & MTK_MML_DISP_DIRECT_LINK_LAYER)
    {
        tmp |= HWC_MML_DISP_DIRECT_LINK_LAYER;
    }

    if (caps & MTK_MML_DISP_DIRECT_DECOUPLE_LAYER)
    {
        tmp |= HWC_MML_DISP_DIRECT_DECOUPLE_LAYER;
    }

    if (caps & MTK_MML_DISP_DECOUPLE_LAYER)
    {
        tmp |= HWC_MML_DISP_DECOUPLE_LAYER;
    }

    if (caps & MTK_MML_DISP_MDP_LAYER)
    {
        tmp |= HWC_MML_DISP_MDP_LAYER;
    }

    if (caps & MTK_MML_DISP_NOT_SUPPORT)
    {
        tmp |= HWC_MML_DISP_NOT_SUPPORT;
    }

    return tmp;
}

static unsigned int mapHwcLayeringCaps2MtkLayeringCaps(int32_t caps)
{
    unsigned int tmp = 0;

    if (caps & HWC_LAYERING_OVL_ONLY)
    {
        tmp |= MTK_LAYERING_OVL_ONLY;
    }

    if (caps & HWC_MDP_RSZ_LAYER)
    {
        tmp |= MTK_MDP_RSZ_LAYER;
    }

    if (caps & HWC_DISP_RSZ_LAYER)
    {
        tmp |= MTK_DISP_RSZ_LAYER;
    }

    if (caps & HWC_MDP_ROT_LAYER)
    {
        tmp |= MTK_MDP_ROT_LAYER;
    }

    if (caps & HWC_MDP_HDR_LAYER)
    {
        tmp |= MTK_MDP_HDR_LAYER;
    }

    if (caps & HWC_NO_FBDC)
    {
        tmp |= MTK_NO_FBDC;
    }

    if (caps & HWC_CLIENT_CLEAR_LAYER)
    {
        tmp |= MTK_CLIENT_CLEAR_LAYER;
    }

    if (caps & HWC_DISP_CLIENT_CLEAR_LAYER)
    {
        tmp |= MTK_DISP_CLIENT_CLEAR_LAYER;
    }

    if (caps & HWC_MML_OVL_LAYER)
    {
        tmp |= MTK_MML_OVL_LAYER;
    }
    return tmp;
}

bool DrmHrt::isEnabled() const
{
    return HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY)->isDisplayHrtSupport();
}

bool DrmHrt::isRPOEnabled() const
{
    return HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY)->isDispRpoSupported();
}

void DrmHrt::fillLayerConfigList(const std::vector<sp<HWCDisplay> >& displays)
{
    for (auto& display : displays)
    {
        if (!display->isConnected())
            continue;

        const uint64_t disp_id = display->getId();
        const std::vector<sp<HWCLayer> >& layers = display->getVisibleLayersSortedByZ();
        unsigned int layers_num = static_cast<unsigned int>(layers.size());

        if (CC_UNLIKELY(disp_id >= DisplayManager::MAX_DISPLAYS)) {
            HWC_LOGE("%s(), disp_id %" PRIu64 " > MAX_DISPLAYS %d", __FUNCTION__, disp_id,
                     DisplayManager::MAX_DISPLAYS);
            continue;
        }
        // reallocate layer_config_list if needed
        if ((m_layer_config_len[disp_id] == 0) || (layers_num > m_layer_config_len[disp_id]))
        {
            if (NULL != m_layer_config_list[disp_id])
                free(m_layer_config_list[disp_id]);

            if (layers_num == 0)
                layers_num = 1;

            m_layer_config_len[disp_id] = layers_num;
            m_layer_config_list[disp_id] = (drm_mtk_layer_config*)calloc(m_layer_config_len[disp_id], sizeof(drm_mtk_layer_config));
            m_layer_mml_info[disp_id] = (mml_frame_info*)calloc(m_layer_config_len[disp_id], sizeof(mml_frame_info));
            if (NULL == m_layer_config_list[disp_id])
            {
                HWC_LOGE("(%" PRIu64 ") Failed to malloc layer_config_list (len=%u)", disp_id, layers_num);
                m_layer_config_len[disp_id] = 0;
                return;
            }
        }

        // init and get PrivateHandle
        drm_mtk_layer_config* layer_config = m_layer_config_list[disp_id];
        mml_frame_info* layer_mml_info = m_layer_mml_info[disp_id];

        for (auto& layer : layers)
        {
            layer_config->ovl_id        = static_cast<uint32_t>(-1);
            layer_config->ext_sel_layer = -1;
            layer_config->src_fmt       =
                (layer->getHwlayerType() == HWC_LAYER_TYPE_DIM) ?
                    mapDispInputColorFormat(HAL_PIXEL_FORMAT_DIM) :
                    mapDispInputColorFormat(layer->getPrivateHandle().format);
            layer_config->compress      = isCompressData(&(layer->getPrivateHandle()));
            layer_config->dataspace     = layer->decideMdpOutDataspace();

            if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM)
            {
                uint32_t mdp_output_format = layer->decideMdpOutputFormat();
                if (mdp_output_format != 0)
                {
                    layer_config->src_fmt = mapDispInputColorFormat(mdp_output_format);
                }
                layer_config->dataspace     = layer->decideMdpOutDataspace();
                layer_config->compress      = layer->decideMdpOutputCompressedBuffers();
            }

            layer_config->src_offset_y  = static_cast<uint32_t>(getSrcTop(layer));
            layer_config->src_offset_x  = static_cast<uint32_t>(getSrcLeft(layer));
            layer_config->dst_offset_y  = static_cast<uint32_t>(getDstTop(layer));
            layer_config->dst_offset_x  = static_cast<uint32_t>(getDstLeft(layer));
            layer_config->dst_width     = static_cast<uint32_t>(getDstWidth(layer));
            layer_config->dst_height    = static_cast<uint32_t>(getDstHeight(layer));
            layer_config->layer_caps    = mapHwcLayeringCaps2MtkLayeringCaps(layer->getLayerCaps());
            layer_config->secure        = isSecure(&(layer->getPrivateHandle()));
            HWC_LOGV("%s, d_xywh:%u,%u,%u,%u,sec:%u", __func__, layer_config->dst_offset_x, layer_config->dst_offset_y,
                    layer_config->dst_width, layer_config->dst_height, layer_config->secure);
            const PrivateHandle& priv_hnd = layer->getPrivateHandle();
            switch(layer->getHwlayerType())
            {
                case HWC_LAYER_TYPE_DIM:
                    layer_config->src_width = static_cast<uint32_t>(getDstWidth(layer));
                    layer_config->src_height = static_cast<uint32_t>(getDstHeight(layer));
                    break;

                case HWC_LAYER_TYPE_MM:
                    layer_config->src_width = static_cast<uint32_t>(WIDTH(layer->getMdpDstRoi()));
                    layer_config->src_height = static_cast<uint32_t>(HEIGHT(layer->getMdpDstRoi()));
                    memcpy(layer_mml_info, layer->getMMLCfg(), sizeof(mml_frame_info));
                    break;

                case HWC_LAYER_TYPE_GLAI:
                    layer_config->src_width  = static_cast<uint32_t>(WIDTH(layer->getGlaiDstRoi()));
                    layer_config->src_height = static_cast<uint32_t>(HEIGHT(layer->getGlaiDstRoi()));
                    layer_config->src_fmt = mapDispInputColorFormat(layer->getGlaiOutFormat());
                    break;

                default:
                    if (layer->getHwlayerType() == HWC_LAYER_TYPE_UI &&
                        (priv_hnd.prexform & HAL_TRANSFORM_ROT_90))
                    {
                        layer_config->src_width  = static_cast<uint32_t>(getSrcHeight(layer));
                        layer_config->src_height = static_cast<uint32_t>(getSrcWidth(layer));
                    }
                    else
                    {
                        layer_config->src_width  = static_cast<uint32_t>(getSrcWidth(layer));
                        layer_config->src_height = static_cast<uint32_t>(getSrcHeight(layer));
                    }
                    break;
            }

            ++layer_config;
            ++layer_mml_info;
        }
    }
}

void DrmHrt::fillDispLayer(const std::vector<sp<HWCDisplay> >& displays)
{
    memset(&m_disp_layer, 0, sizeof(drm_mtk_layering_info));
    m_disp_layer.hrt_num = -1;
    for (auto& display : displays)
    {
        const uint64_t disp_id = display->getId();
        m_disp_layer.gles_head[disp_id] = -1;
        m_disp_layer.gles_tail[disp_id] = -1;
    }

    // prepare drm_mtk_layering_info for ioctl
    for (auto& display : displays)
    {
        if (!display->isValid() ||
            display->getMirrorSrc() != -1 ||
            HWCMediator::getInstance().getOvlDevice(display->getId())->getType() != OVL_DEVICE_TYPE_OVL)
        {
            continue;
        }

        sp<DrmDevice> drm_dev = reinterpret_cast<DrmDevice*>(HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY).get());
        const uint64_t disp_id = display->getId();
        const size_t disp_input = drm_dev->getHrtIndex(disp_id);
        if (CC_UNLIKELY(disp_input >= DisplayManager::MAX_DISPLAYS)) {
            HWC_LOGE("%s(), disp_input %zu > MAX_DISPLAYS %d", __FUNCTION__, disp_input, DisplayManager::MAX_DISPLAYS);
            continue;
        }

        const unsigned int layers_num = static_cast<unsigned int>(display->getVisibleLayersSortedByZ().size());

        m_disp_layer.input_config[disp_input] = m_layer_config_list[disp_id];
        m_disp_layer.mml_cfg[disp_input] = m_layer_mml_info[disp_id];

        switch (disp_id) {
            case HWC_DISPLAY_PRIMARY:
                {
                    m_disp_layer.disp_mode[disp_input] = static_cast<int>(drm_dev->getDrmSessionMode(disp_id));
                    m_disp_layer.disp_mode_idx[disp_input] = static_cast<int>(display->getActiveConfig());
                }
                break;

            case HWC_DISPLAY_EXTERNAL:
                m_disp_layer.disp_mode[disp_input] = HWC_DISP_SESSION_DIRECT_LINK_MODE;
                break;

            case HWC_DISPLAY_VIRTUAL:
                m_disp_layer.disp_mode[disp_input] = HWC_DISP_SESSION_DECOUPLE_MODE;
                break;

            default:
                HWC_LOGE("%s: Unknown disp_id(%" PRIu64 ")", __func__, disp_id);
        }

        m_disp_layer.layer_num[disp_input] = static_cast<int>(
            (m_layer_config_len[disp_id] < layers_num) ? m_layer_config_len[disp_id] : layers_num);

        display->getGlesRange(
            &m_disp_layer.gles_head[disp_input],
            &m_disp_layer.gles_tail[disp_input]);
        HWC_LOGV("%s disp:%" PRIu64 " m_disp_layer.gles_head[disp_input]:%d, m_disp_layer.gles_tail[disp_input]:%d",
            __func__, disp_id, m_disp_layer.gles_head[disp_input],m_disp_layer.gles_tail[disp_input] );
    }
}

void DrmHrt::fillLayerInfoOfDispatcherJob(const std::vector<sp<HWCDisplay> >& displays)
{
    // DbgLogger logger(DbgLogger::TYPE_HWC_LOG, 'D',"fillLayerInfoOfDispatcherJob()");
    for (auto& display : displays)
    {
        const uint64_t disp_id = display->getId();
        DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(disp_id);

        if (!display->isConnected() || NULL == job || display->getMirrorSrc() != -1)
        {
            HWC_LOGV("%s(), job:%p display->getMirrorSrc():%d", __FUNCTION__, job, display->getMirrorSrc());
            continue;
        }

        // only support two display at the same time
        // index 0: primary display; index 1: secondry display(MHL or vds)
        // fill display info
        sp<DrmDevice> drm_dev = reinterpret_cast<DrmDevice*>(HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY).get());
        const size_t disp_input = drm_dev->getHrtIndex(disp_id);
        if (CC_UNLIKELY(disp_input >= DisplayManager::MAX_DISPLAYS)) {
            HWC_LOGE("%s(), disp_input %zu > MAX_DISPLAYS %d", __FUNCTION__, disp_input, DisplayManager::MAX_DISPLAYS);
            continue;
        }

        // fill layer info
        if (m_layer_config_len[disp_id] > m_hrt_config_len[disp_input])
        {
            unsigned int layers_num = m_layer_config_len[disp_id];
            if (NULL != m_hrt_config_list[disp_input])
                delete m_hrt_config_list[disp_input];

            m_hrt_config_len[disp_input] = layers_num;
            m_hrt_config_list[disp_input] = new HrtLayerConfig[layers_num];
            if (NULL == m_hrt_config_list[disp_input])
            {
                HWC_LOGE("(%" PRIu64 ") Failed to malloc hrt_config_list (len=%u)", disp_id, layers_num);
                m_hrt_config_len[disp_input] = 0;
                return;
            }
        }
        for (unsigned int i = 0; i < m_layer_config_len[disp_id]; i++)
        {
            if (HWCMediator::getInstance().getOvlDevice(display->getId())->getType() == OVL_DEVICE_TYPE_OVL)
            {
                m_hrt_config_list[disp_input][i].ovl_id = m_disp_layer.input_config[disp_input][i].ovl_id;
                m_hrt_config_list[disp_input][i].ext_sel_layer = m_disp_layer.input_config[disp_input][i].ext_sel_layer;
            }
            else
            {
                m_hrt_config_list[disp_input][i].ovl_id = 0;
                m_hrt_config_list[disp_input][i].ext_sel_layer = -1;
            }
        }
        job->layer_info.hrt_config_list = m_hrt_config_list[disp_input];

        if (m_disp_layer.input_config[disp_input] == NULL)
        {
            const std::vector<sp<HWCLayer> >& layers = display->getVisibleLayersSortedByZ();
            job->layer_info.max_overlap_layer_num = -1;
            job->layer_info.hrt_weight = 0;
            job->layer_info.hrt_idx    = 0;
            const int&& layers_num = static_cast<int>(layers.size());
            job->layer_info.gles_head = layers_num ? 0 : -1;
            job->layer_info.gles_tail = layers_num - 1;
            HWC_LOGV("%s(), disp:%" PRIu64 " gles_head:%d gles_tail:%d hrt:%u,%u with no hrt config", __FUNCTION__,
                disp_id, job->layer_info.gles_head, job->layer_info.gles_tail,
                job->layer_info.hrt_weight, job->layer_info.hrt_idx);
            continue;
        }
        else
        {
            job->layer_info.max_overlap_layer_num = m_disp_layer.hrt_num;
            job->layer_info.hrt_weight = m_disp_layer.hrt_weight;
            job->layer_info.hrt_idx = m_disp_layer.hrt_idx;
            job->layer_info.gles_head = m_disp_layer.gles_head[disp_input];
            job->layer_info.gles_tail = m_disp_layer.gles_tail[disp_input];
            HWC_LOGV("%s(), disp:%" PRIu64 " gles_head:%d gles_tail:%d hrt:%u,%u", __FUNCTION__,
                disp_id, job->layer_info.gles_head, job->layer_info.gles_tail,
                job->layer_info.hrt_weight, job->layer_info.hrt_idx);
        }

        for (size_t i = 0; i < display->getVisibleLayersSortedByZ().size(); ++i)
        {
            if (static_cast<int32_t>(i) >= m_disp_layer.layer_num[disp_input])
                break;

            auto& layer = display->getVisibleLayersSortedByZ()[i];
            layer->setLayerCaps(mapMtkLayeringCaps2HwcLayeringCaps(m_disp_layer.input_config[disp_input][i].layer_caps));
            if (layer->getLayerCaps() & HWC_DISP_CLIENT_CLEAR_LAYER)
            {
                layer->setHWCRequests(layer->getHWCRequests() | HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET);
            }
        }
        // for (int32_t i = 0 ; i < m_disp_layer.layer_num[disp_input]; ++i)
        //    logger.printf("i:%d ovl_id:%d caps:%d, ", i, m_disp_layer.input_config[disp_input][i].ovl_id, m_disp_layer.input_config[disp_input][i].layer_caps);
    }
}

void DrmHrt::printQueryValidLayerResult()
{
    for (int32_t i = 0; i < 3; ++i)
    {
        if (m_disp_layer.layer_num[i] == 0)
        {
            continue;
        }

        m_hrt_result.str("");
        m_hrt_result << "[HRT DRM]";
        m_hrt_result << " [(" << i << ") mode:" << m_disp_layer.disp_mode_idx[i];
        m_hrt_result << " g(" << m_disp_layer.gles_head[i] << ", " << m_disp_layer.gles_tail[i] << ")]";
        for (int32_t j = 0; j < m_disp_layer.layer_num[i]; ++j)
        {
            const auto& cfg = m_disp_layer.input_config[i][j];
            m_hrt_result << " [(" << j <<
                ") s_wh:" << cfg.src_width << "," << cfg.src_height <<
                " d_xywh:" << cfg.dst_offset_x << "," << cfg.dst_offset_y << "," << cfg.dst_width << ","<< cfg.dst_height <<
                " caps:" << std::hex << cfg.layer_caps << "]" << std::dec;
        }
        HWC_LOGD("%s", m_hrt_result.str().c_str());
    }
}

bool DrmHrt::queryValidLayer()
{
    return HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY)->queryValidLayer(&m_disp_layer);
}
