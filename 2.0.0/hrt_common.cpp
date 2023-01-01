#define DEBUG_LOG_TAG "HRT"

#include "hrt_common.h"

#include <vector>

#include "overlay.h"
#include "dispatcher.h"
#include "hwc2.h"

#ifndef MTK_HWC_USE_DRM_DEVICE
#include "legacy/hrt.h"
#else
#include "drm/drmhrt.h"
#endif

bool HrtCommon::isEnabled() const
{
    return false;
}

bool HrtCommon::isRPOEnabled() const
{
    return false;
}

void HrtCommon::setCompType(const std::vector<sp<HWCDisplay> >& displays)
{
    for (auto& display : displays)
    {
        DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(display->getId());

        if (!display->isConnected() || NULL == job || display->getMirrorSrc() != -1)
            continue;

        // only support two display at the same time
        // index 0: primary display; index 1: secondry display(MHL or vds)
        // fill display info
        int32_t gles_head = -1, gles_tail = -1;
        display->getGlesRange(&gles_head, &gles_tail);
        if (gles_head != job->layer_info.gles_head || gles_tail != job->layer_info.gles_tail)
        {
            gles_head = job->layer_info.gles_head;
            gles_tail = job->layer_info.gles_tail;
            display->setGlesRange(gles_head, gles_tail);
        }
    }
}

void HrtCommon::dump(String8* str)
{
    str->appendFormat("%s\n", m_hrt_result.str().c_str());
}

void HrtCommon::printQueryValidLayerResult()
{
    m_hrt_result.str("");
    m_hrt_result << "[HRT Interface]";
    HWC_LOGD("%s", m_hrt_result.str().c_str());
}

void HrtCommon::modifyMdpDstRoiIfRejectedByRpo(const std::vector<sp<HWCDisplay> >& displays)
{
    for (auto& disp : displays)
    {
        if (!disp->isValid())
            continue;

        for (auto& layer : disp->getVisibleLayersSortedByZ())
        {
            if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM &&
                (WIDTH(layer->getMdpDstRoi()) != WIDTH(layer->getDisplayFrame()) ||
                HEIGHT(layer->getMdpDstRoi()) != HEIGHT(layer->getDisplayFrame())) &&
                (layer->getLayerCaps() & HWC_DISP_RSZ_LAYER) == 0)
            {
                layer->editMdpDstRoi().left = 0;
                layer->editMdpDstRoi().top = 0;
                layer->editMdpDstRoi().right = WIDTH(layer->getDisplayFrame());
                layer->editMdpDstRoi().bottom = HEIGHT(layer->getDisplayFrame());
            }
        }
    }
}

void HrtCommon::simpleLayeringRule(const std::vector<sp<HWCDisplay> >& displays)
{
    for (auto& hwc_display : displays)
    {
        if (!hwc_display->isValid())
            continue;

        if (hwc_display->getMirrorSrc() != -1)
            continue;

        DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(hwc_display->getId());

        if (NULL == job)
            continue;

        int32_t gles_head = -1, gles_tail = -1;
        hwc_display->getGlesRange(&gles_head, &gles_tail);
        const bool only_hwc_comp = (gles_tail == -1) && (gles_head == -1);
        if (!only_hwc_comp &&
            ((gles_head | gles_tail) < 0 || (gles_head > gles_tail)))
        {
            HWC_LOGE("wrong GLES range (%d,%d)", gles_head, gles_tail);
            abort();
        }

        const int num_hwc_layers = static_cast<int>(hwc_display->getVisibleLayersSortedByZ().size());
        int max_layer = static_cast<int>(job->num_layers);
        const int gles_count = only_hwc_comp ? 0 : (gles_tail - gles_head + 1);
        const int hwc_count = num_hwc_layers - gles_count;
        const int committed_count = only_hwc_comp ? hwc_count : hwc_count + 1;

        if (committed_count > max_layer)
        {
            const std::vector<sp<HWCLayer> >& layers = hwc_display->getVisibleLayersSortedByZ();
            bool has_clear_client_layers = false;
            for (size_t i = 1; i < layers.size() - 1; ++i)
            {
                sp<HWCLayer> layer = layers[i];
                if (layer->getLayerCaps() & HWC_CLIENT_CLEAR_LAYER)
                {
                    has_clear_client_layers = true;
                    layer->setHWCRequests(layer->getHWCRequests() | HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET);
                    max_layer--;
                }
                if (max_layer == 1)
                    break;
            }

            const int over_layer_count = committed_count - max_layer;
            if (has_clear_client_layers)
            {
                gles_tail = static_cast<int>(layers.size()) - 1;
                gles_head = 0;
            }
            else if (only_hwc_comp)
            {
                gles_tail = num_hwc_layers - 1;
                gles_head = gles_tail - over_layer_count;
            }
            else
            {
                const int hwc_num_after_gles_tail = num_hwc_layers - 1 - gles_tail;
                const int hwc_num_before_gles_head = gles_head;
                const int excess_layer = over_layer_count > hwc_num_after_gles_tail ?
                                         over_layer_count - hwc_num_after_gles_tail : 0;
                if (excess_layer > hwc_num_before_gles_head)
                {
                    HWC_LOGE("wrong GLES head range (%d,%d) (%d,%d)", gles_head, gles_tail, hwc_count, max_layer);
                    abort();
                }
                gles_tail = excess_layer == 0 ?
                            (gles_tail + over_layer_count) : (num_hwc_layers - 1);
                gles_head -= excess_layer;
            }
            hwc_display->setGlesRange(gles_head, gles_tail);
        }
    }
}

void HrtCommon::fillLayerInfoOfDispatcherJob(const std::vector<sp<HWCDisplay> >& displays)
{
    static unsigned int hrt_idx = 0;
    for (auto& hwc_display : displays)
    {
        if (!hwc_display->isValid())
            continue;

        if (hwc_display->getMirrorSrc() != -1)
            continue;

        const uint64_t disp_id = hwc_display->getId();
        DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(disp_id);

        if (!job)
        {
            continue;
        }

        // fill layer info
        const std::vector<sp<HWCLayer> >& layers = hwc_display->getVisibleLayersSortedByZ();
        const unsigned int layers_num = static_cast<unsigned int>(layers.size());
        if (layers_num > m_hrt_config_len[disp_id])
        {
            if (NULL != m_hrt_config_list[disp_id])
                delete m_hrt_config_list[disp_id];

            m_hrt_config_len[disp_id] = layers_num;
            m_hrt_config_list[disp_id] = new HrtLayerConfig[layers_num];
            if (NULL == m_hrt_config_list[disp_id])
            {
                HWC_LOGE("(%" PRIu64 ") Failed to malloc hrt_config_list (len=%d)", disp_id, layers_num);
                m_hrt_config_len[disp_id] = 0;
                return;
            }
        }

        int32_t gles_head = -1, gles_tail = -1;
        hwc_display->getGlesRange(&gles_head, &gles_tail);
        job->layer_info.gles_head = gles_head;
        job->layer_info.gles_tail = gles_tail;
        unsigned int ovl_index = 0;
        unsigned int clear_layer_num = hwc_display->getClientClearLayerNum();
        bool has_clear_client_layers = clear_layer_num > 0;
        int32_t layer_idx = 0;
        for (size_t i = 0; i < layers.size(); ++i, ++layer_idx)
        {
            sp<HWCLayer> layer = layers[i];
            if (has_clear_client_layers)
            {
                if (layer_idx >= gles_head || layer_idx <= gles_tail)
                {
                    if (layer->getHWCRequests() & HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET)
                    {
                        m_hrt_config_list[disp_id][i].ovl_id = ovl_index;
                        HWC_LOGV("%s(), layer%zu id:%u dev clear",
                                 __FUNCTION__, i, m_hrt_config_list[disp_id][i].ovl_id);
                        ovl_index++;
                        clear_layer_num--;
                    }
                    else
                    {
                        m_hrt_config_list[disp_id][i].ovl_id = ovl_index + clear_layer_num;
                        HWC_LOGV("%s(), layer%zu id:%u cli", __FUNCTION__, i, m_hrt_config_list[disp_id][i].ovl_id);
                    }
                }
                else
                {
                    m_hrt_config_list[disp_id][i].ovl_id = ovl_index;
                    HWC_LOGV("%s(), layer%zu id:%u dev", __FUNCTION__, i, m_hrt_config_list[disp_id][i].ovl_id);
                    ovl_index++;
                }
            }
            else
            {
                HWC_LOGV("%s(), layer%zu id:%u ", __FUNCTION__, i, ovl_index);
                m_hrt_config_list[disp_id][i].ovl_id = ovl_index;
                if ( layer_idx < gles_head || layer_idx >= gles_tail)
                {
                    ovl_index++;
                }
            }
            m_hrt_config_list[disp_id][i].ext_sel_layer = -1;
        }
        job->layer_info.hrt_config_list = m_hrt_config_list[disp_id];
        job->layer_info.max_overlap_layer_num = -1;
        job->layer_info.hrt_weight = 0;
        job->layer_info.hrt_idx = hrt_idx++;

        HWC_LOGV("%s(), disp:%" PRIu64 " gles_head:%d gles_tail:%d hrt:%u,%u", __FUNCTION__,
                 disp_id, job->layer_info.gles_head, job->layer_info.gles_tail,
                 job->layer_info.hrt_weight, job->layer_info.hrt_idx);
    }
}

void HrtCommon::run(std::vector<sp<HWCDisplay> >& displays, const bool& is_skip_validate)
{
    updateGlesRangeForDisplaysBeforHrt(displays);
    if (0 == isEnabled())
    {
        HrtCommon::simpleLayeringRule(displays);
        updateGlesRangeHwTypeForDisplays(displays);
        HrtCommon::fillLayerInfoOfDispatcherJob(displays);
        return;
    }

    if (is_skip_validate)
    {
        updateGlesRangeHwTypeForDisplays(displays);
        fillLayerInfoOfDispatcherJob(displays);
        return;
    }

    fillLayerConfigList(displays);

    fillDispLayer(displays);

    if (queryValidLayer())
    {
        fillLayerInfoOfDispatcherJob(displays);
        setCompType(displays);
        if (isRPOEnabled())
        {
            modifyMdpDstRoiIfRejectedByRpo(displays);
            printQueryValidLayerResult();
        }
        updateGlesRangeHwTypeForDisplays(displays);
    }
    else
    {
        HWC_LOGE("%s: an error when hrt calculating!", __func__);

        for (auto& display : displays)
        {
            if (!display->isConnected())
                continue;

            DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(display->getId());

            if (NULL == job || display->getMirrorSrc() != -1)
                continue;

            job->layer_info.max_overlap_layer_num = -1;
        }
    }
}

void HrtCommon::updateGlesRangeForDisplaysBeforHrt(const std::vector<sp<HWCDisplay> >& displays) {
    for (auto& display : displays)
    {
        if (!display->isConnected())
            continue;

        display->updateGlesRange();
    }
}

void HrtCommon::updateGlesRangeHwTypeForDisplays(const std::vector<sp<HWCDisplay> >& displays) {
    for (auto& display : displays)
    {
        if (!display->isConnected())
            continue;

        display->updateGlesRangeHwType();
    }
}

HrtCommon* createHrt()
{
#ifndef MTK_HWC_USE_DRM_DEVICE
    return new Hrt();
#else
    return new DrmHrt();
#endif
}
