#define DEBUG_LOG_TAG "HWCDisplay"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "hwcdisplay.h"

#include <algorithm>
#include <thread>

#include <hwc_feature_list.h>

#include "hwc2_defs.h"
#include "platform_wrap.h"
#include "overlay.h"
#include "dispatcher.h"
#include "pq_interface.h"
#include "data_express.h"

#ifdef MTK_HDR_SET_DISPLAY_COLOR
// The flag must be sync with kernel source code
#define MTK_HDR10P_PROPERTY_FLAG 2
#endif

void findGlesRange(const std::vector<sp<HWCLayer> >& layers, int32_t* head, int32_t* tail)
{
    auto&& head_iter = find_if(layers.begin(), layers.end(),
        [](const sp<HWCLayer>& layer)
        {
            return layer->getCompositionType() == HWC2_COMPOSITION_CLIENT;
        });

    auto&& tail_iter = find_if(layers.rbegin(), layers.rend(),
        [](const sp<HWCLayer>& layer)
        {
            return layer->getCompositionType() == HWC2_COMPOSITION_CLIENT;
        });

    *head = head_iter != layers.end() ? static_cast<int32_t>(head_iter - layers.begin()) : -1;
    *tail = tail_iter != layers.rend() ? static_cast<int32_t>(layers.rend() - tail_iter) - 1 : -1;
}

inline uint32_t extendMDPCapacity(
    const std::vector<sp<HWCLayer> >& /*layers*/, const uint32_t& /*mm_layer_num*/,
    const uint32_t& camera_layer_num, const uint32_t& /*video_layer_num*/)
{
    if (Platform::getInstance().m_config.extend_mdp_capacity)
    {
        // rule 1: no layer came from camera
        if (camera_layer_num != 0)
        {
            return 0;
        }

        // rule 2: Primary diaplay only
        if (DisplayManager::getInstance().getDisplayData(HWC_DISPLAY_EXTERNAL)->connected ||
            DisplayManager::getInstance().getDisplayData(HWC_DISPLAY_VIRTUAL)->connected)
        {
            return 0;
        }

        return 1;
    }

    return 0;
}

HWCDisplay::HWCDisplay(const uint64_t& disp_id, const int32_t& type, const sp<IOverlayDevice>& ovl)
    : m_type(type)
    , m_outbuf(nullptr)
    , m_is_validated(false)
    , m_disp_id(disp_id)
    , m_active_config(0)
    , m_config_changed(false)
    , m_gles_head(-1)
    , m_gles_tail(-1)
    , m_retire_fence_fd(-1)
    , m_mir_src(-1)
    , m_power_mode(HWC2_POWER_MODE_ON)
    , m_color_transform_hint(HAL_COLOR_TRANSFORM_IDENTITY)
    , m_color_mode(HAL_COLOR_MODE_NATIVE)
    , m_prev_color_mode(HAL_COLOR_MODE_NATIVE)
    , m_render_intent(HAL_RENDER_INTENT_COLORIMETRIC)
    , m_prev_render_intent(HAL_RENDER_INTENT_COLORIMETRIC)
    , m_hdr_type(MTK_METADATA_TYPE_NONE)
    , m_prev_hdr_type(MTK_METADATA_TYPE_NONE)
    , m_presented_pq_mode_id(DEFAULT_PQ_MODE_ID)
    , m_pq_mode_id(DEFAULT_PQ_MODE_ID)
    , m_need_av_grouping(false)
    , m_use_gpu_composition(false)
    , m_color_transform_ok(true)
    , m_color_transform(new ColorTransform(HAL_COLOR_TRANSFORM_IDENTITY, false, false))
    , m_ccorr_state(NOT_FBT_ONLY)
    , m_prev_available_input_layer_num(0)
    , m_vali_present_state(HWC_VALI_PRESENT_STATE_PRESENT_DONE)
    , m_is_visible_layer_changed(false)
    , m_unpresent_count(0)
    , m_prev_unpresent_count(0)
#ifdef MTK_IN_DISPLAY_FINGERPRINT
    // for LED HBM (High Backlight Mode) control
    , m_is_HBM(false)
#endif
    , m_client_clear_layer_num(0)
    , m_dispathcer_job_status(HWC_DISPACHERJOB_VALID)
    , m_last_app_game_pq(false)
    , m_is_m4u_sec_inited(false)
    , m_num_video_layer(0)
    , m_num_camera_layer(0)
    , m_prev_extend_sf_target_time(0)
{
    switch (disp_id)
    {
        case HWC_DISPLAY_PRIMARY:
            hwc2_layer_t id = 0;
            createLayer(&id, true);
            m_ct = getLayer(id);
    }
    initColorHistogram(ovl);
    populateColorModeAndRenderIntent(ovl);

    m_perf_ovl_atomic_work_time_str = std::string("pm_atomic_reserved_") + std::to_string(disp_id);
    m_perf_extend_sf_target_time_str = std::string("pm_extend_sf_target_time_") +
            std::to_string(disp_id);
}

void HWCDisplay::init()
{
    m_unpresent_count = 0;
    m_prev_unpresent_count = 0;

    switch (getId())
    {
        case HWC_DISPLAY_EXTERNAL:
            {
                hwc2_layer_t id = 0;
                createLayer(&id, true);
                m_ct = getLayer(id);
                m_is_validated = false;
                m_gles_head = -1;
                m_gles_tail = -1;
                m_retire_fence_fd = -1;
                m_mir_src = -1;
                // external display will not present until SF setPowerMode ON
                m_power_mode = HWC2_POWER_MODE_OFF;
                m_color_transform_hint = HAL_COLOR_TRANSFORM_IDENTITY;
                m_color_mode = HAL_COLOR_MODE_NATIVE;
                m_use_gpu_composition = false;
                m_need_av_grouping = false;
                m_color_transform_ok = true;
                m_prev_available_input_layer_num = 0;
                m_vali_present_state = HWC_VALI_PRESENT_STATE_PRESENT_DONE;
                m_is_visible_layer_changed = false;
                m_dispathcer_job_status = HWC_DISPACHERJOB_VALID;
                m_need_free_fb_cache = false;
            }
            break;

        case HWC_DISPLAY_VIRTUAL:
            {
                m_outbuf = new HWCBuffer(getId(), -1, false);
                hwc2_layer_t id = 0;
                createLayer(&id, true);
                m_ct = getLayer(id);
                m_is_validated = false;
                m_gles_head = -1;
                m_gles_tail = -1;
                m_retire_fence_fd = -1;
                m_mir_src = -1;
                m_power_mode = HWC2_POWER_MODE_ON;
                m_color_transform_hint = HAL_COLOR_TRANSFORM_IDENTITY;
                m_color_mode = HAL_COLOR_MODE_NATIVE;
                m_use_gpu_composition = false;
                m_need_av_grouping = false;
                m_color_transform_ok = true;
                m_prev_available_input_layer_num = 0;
                m_vali_present_state = HWC_VALI_PRESENT_STATE_PRESENT_DONE;
                m_is_visible_layer_changed = false;
                m_dispathcer_job_status = HWC_DISPACHERJOB_VALID;
                m_need_free_fb_cache = false;
            }
            break;
    }
}

void HWCDisplay::initPrevCompTypes()
{
    auto&& layers = getVisibleLayersSortedByZ();
    m_prev_comp_types.resize(layers.size());
    for (size_t i = 0; i < m_prev_comp_types.size(); ++i)
        m_prev_comp_types[i] = layers[i]->getCompositionType();
}

int32_t HWCDisplay::setColorTransform(const float* matrix, const int32_t& hint)
{
    m_color_transform_hint = hint;
    m_color_transform = new ColorTransform(matrix, hint, true);

    if (!HwcFeatureList::getInstance().getFeature().is_support_pq ||
        !Platform::getInstance().m_config.support_color_transform ||
        getId() == HWC_DISPLAY_VIRTUAL)
    {
        m_color_transform_ok = (hint == HAL_COLOR_TRANSFORM_IDENTITY);
        m_color_transform->dirty = false;
    }
    else
    {
#ifdef USES_PQSERVICE
        if (getId() == HWC_DISPLAY_PRIMARY)
        {
            m_color_transform_ok = getPqDevice()->setColorTransform(matrix, hint);
            HWC_LOGI("(%" PRIu64 ") %s hint:%d ok:%d", getId(), __func__, hint, m_color_transform_ok);
            if (!m_color_transform_ok)
            {
                m_color_transform = new ColorTransform(HAL_COLOR_TRANSFORM_IDENTITY, true, false);
            }
        }
        else
        {
            m_color_transform_ok = false;
        }
#else
        m_color_transform_ok = (hint == HAL_COLOR_TRANSFORM_IDENTITY);
#endif
    }
    return m_color_transform_ok ? HWC2_ERROR_NONE : HWC2_ERROR_UNSUPPORTED;
}

void HWCDisplay::setJobDisplayOrientation()
{
    auto&& layers = getVisibleLayersSortedByZ();
    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(getId());
    //HWC decides how MDP rotates the display WDMA output buffer to fit TV out in mirror path,
    //so orienatation of source and sink display is needed in the original solution. Unfortunately,
    //SF does NOT tell HWC about display orientation, so we must figure out the other solution without modification AOSP code.
    //The importance is how many degrees to rotate the WDMA buffer not the sigle display orientation.
    //Therefore, the single layer's transform on the source and sink display can be used in this case.
    //SF provides how a single layer should be rotated on the source and sink display to HWC,
    //and the rotation can be used for the WDMA output buffer,too.
    if (job != nullptr)
    {
        // job->disp_ori_rot has initialized with 0 when job create
        if (layers.size())
        {
            for (auto& layer : layers)
            {
                if (layer->getSFCompositionType() != HWC2_COMPOSITION_SOLID_COLOR)
                {
                    job->disp_ori_rot = layer->getTransform();
                    break;
                }
            }
        }
    }
}

void HWCDisplay::setJobDisplayData()
{
    const uint64_t disp_id = getId();
    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(disp_id);

    if (job != nullptr)
    {
        job->disp_data = DisplayManager::getInstance().getDisplayData(disp_id, m_active_config);
        job->active_config = m_active_config;

        if (m_need_free_fb_cache)
        {
            m_need_free_fb_cache = false;
            DataPackage& package = DataExpress::getInstance().requestPackage(disp_id, job->sequence);
            package.m_need_free_fb_cache = true;
        }

        if (m_msync_2_0_enable_changed)
        {
            m_msync_2_0_enable_changed = false;

            DataPackage& package = DataExpress::getInstance().requestPackage(disp_id, job->sequence);
            if (!package.m_msync2_data)
            {
                package.m_msync2_data = std::make_optional<MSync2Data>();
            }
            package.m_msync2_data->m_msync2_enable = m_msync_2_0_enable;
        }

        if (m_msync_2_0_param_table)
        {
            DataPackage& package = DataExpress::getInstance().requestPackage(disp_id, job->sequence);
            if (!package.m_msync2_data)
            {
                package.m_msync2_data = std::make_optional<MSync2Data>();
            }
            package.m_msync2_data->m_param_table = m_msync_2_0_param_table;

            m_msync_2_0_param_table = nullptr;
        }

        if (m_sf_target_ts > 0) // only mtk system image will set this value
        {
            if (m_sf_target_ts == m_prev_sf_target_ts)
            {
                HWC_LOGD("sf did not update sf target ts");
            }
            else
            {
                m_prev_sf_target_ts = m_sf_target_ts;

                const nsecs_t cur_time = systemTime();
                if (abs(m_sf_target_ts - cur_time) > ms2ns(50))
                {
                    HWC_LOGW("sf_target_ts weird %" PRId64", cur_time %" PRId64, m_sf_target_ts, cur_time);
                }
                else
                {
                    job->sf_target_ts = m_sf_target_ts;
                    job->present_after_ts = m_sf_target_ts - job->disp_data->refresh;

                    if (ATRACE_ENABLED())
                    {
                        char atrace_tag[128];
                        const nsecs_t cur_time = systemTime();
                        if (snprintf(atrace_tag, sizeof(atrace_tag),
                                     "(%" PRIu64 ")sf ts: %" PRId64 " cur %" PRId64 " diff %" PRId64 ".%" PRId64 "ms",
                                     m_disp_id,
                                     m_sf_target_ts, cur_time,
                                     (m_sf_target_ts - cur_time) / 1000000,
                                     (m_sf_target_ts - cur_time) % 1000000) > 0)
                        {
                            HWC_ATRACE_NAME(atrace_tag);
                        }
                    }
                }
            }
        }

        job->cpu_set = updateCpuSet();
    }
}

void HWCDisplay::validate()
{
    if (!isConnected())
    {
        HWC_LOGE("%s: the display(%" PRIu64 ") is not connected!", __func__, m_disp_id);
        return;
    }

    setValiPresentState(HWC_VALI_PRESENT_STATE_VALIDATE, __LINE__);
    m_is_validated = true;
    m_client_clear_layer_num = 0;

    auto&& layers = getVisibleLayersSortedByZ();

    m_hdr_type = MTK_METADATA_TYPE_NONE;
    for (auto& layer : layers)
    {
        layer->initValidate();
        m_hdr_type |= layer->getHdrType();
    }
    updatePqModeId();

#ifdef MTK_IN_DISPLAY_FINGERPRINT
    setIsHBM(false);
    for (auto& layer : layers)
    {
        if (layer->getLayerUsage() & HWC_LAYER_USAGE_HBM)
        {
            setIsHBM(true);
            break;
        }
    }
#endif

    const uint64_t disp_id = getId();
    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(disp_id);
    const unsigned int ovl_layer_max = (job != NULL) ? job->num_layers : 0;
    const bool force_gpu_compose = isForceGpuCompose();
    bool fallback_gpu_compose = false;

    if (disp_id == HWC_DISPLAY_VIRTUAL)
    {
        if (getOutbuf() != NULL && getOutbuf()->getHandle() != nullptr)
        {
            if (getSecure() && !usageHasProtectedOrSecure(getOutbuf()->getPrivateHandle().usage))
            {
                fallback_gpu_compose = true;
                HWC_LOGE("%s: the display(%" PRIu64 ") is secure virtual display, but outbuf is not secure!", __func__, m_disp_id);
            }
        }
        else
        {
            fallback_gpu_compose = true;
            HWC_LOGE("%s: the display(%" PRIu64 ") is virtual display and is validated without outbuf ready!", __func__, m_disp_id);
        }
    }

    if (m_dispathcer_job_status == HWC_DISPACHERJOB_INVALID_DROPJOB)
    {
        HWC_ATRACE_NAME("drop_job");
        for (auto& layer : layers)
            layer->setHwlayerType(HWC_LAYER_TYPE_MM, __LINE__, HWC_COMP_FILE_HWCD);
    }
    else if (getMirrorSrc() != -1)
    {
        for (auto& layer : layers)
        {
            layer->setHwlayerType(HWC_LAYER_TYPE_WORMHOLE, __LINE__, HWC_COMP_FILE_HWCD);
        }
    }
    else if (force_gpu_compose || fallback_gpu_compose)
    {
        for (auto& layer : layers)
            layer->setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCD);
    }
    else
    {
        const hwc_rect_t disp_bound = {0, 0, getWidth(m_active_config), getHeight(m_active_config)};
        int32_t pq_mode_id = m_pq_mode_id;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            auto& layer = layers[i];
            // force full invalidate
            if (disp_id == HWC_DISPLAY_PRIMARY)
            {
                if (static_cast<int32_t>(i) != Platform::getInstance().m_config.force_pq_index)
                {
                    layer->setForcePQ(false);
                }
                else
                {
                    layer->setForcePQ(true);
                }
            }
            layer->boundaryCut(disp_bound);
            layer->validate(pq_mode_id);
        }
        offloadMMtoClient();
    }
    for (size_t i = 0; i < layers.size(); ++i)
    {
        layers[i]->completeLayerCaps(i == 0);
    }

/*
    auto&& print_layers = m_layers;
    for (auto& kv : print_layers)
    {
        auto& layer = kv.second;
        auto& display_frame = layer->getDisplayFrame();
        HWC_LOGD("(%d) layer id:%" PRIu64 " hnd:%x z:%d hwlayer type:%s(%s,%s) line:%d displayf:[%d,%d,%d,%d] tr:%d",
            getId(),
            layer->getId(),
            layer->getHandle(),
            layer->getZOrder(),
            getCompString(layer->getCompositionType()),
            getHWLayerString(layer->getHwlayerType()),
            getCompString(layer->getSFCompositionType()),
            layer->getHwlayerTypeLine(),
            display_frame.left,
            display_frame.top,
            display_frame.right,
            display_frame.bottom,
            layer->getTransform()
        );
    }
*/
    HWC_LOGD("(%" PRIu64 ") VAL/l:%zu/max:%u/fg:%d,%d", getId(), layers.size(), ovl_layer_max, force_gpu_compose, fallback_gpu_compose);
}

void HWCDisplay::offloadMMtoClient()
{
    auto&& layers = getVisibleLayersSortedByZ();
    const uint32_t MAX_MM_NUM = 1;
    uint32_t mm_layer_num = 0;
    m_num_camera_layer = 0;
    m_num_video_layer = 0;

    for (auto& layer : layers)
    {
        if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM)
            ++mm_layer_num;
    }

    // if extening sf target time is enable, we need to count the camera and video layer for
    // this frame, so we skip this check first.
    if (!(Platform::getInstance().m_config.plat_switch &
            (HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_VIDEO |
            HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_CAMERA)))
    {
        if (mm_layer_num <= MAX_MM_NUM)
            return;
    }
    for (auto& layer : layers)
    {
        switch (getGeTypeFromPrivateHandle(&layer->getPrivateHandle()))
        {
            case GRALLOC_EXTRA_BIT_TYPE_CAMERA:
                ++m_num_camera_layer;
                break;

            case GRALLOC_EXTRA_BIT_TYPE_VIDEO:
                ++m_num_video_layer;
                break;
        }
    }

    if (mm_layer_num <= MAX_MM_NUM)
        return;

    uint32_t mm_ui_num = mm_layer_num - m_num_camera_layer - m_num_video_layer;
    // for MM_UI layer too much case, we will offload MM_UI layer into Client
    if (mm_ui_num >= 2 && mm_layer_num >= 3 && layers.size() >= 4)
    {
        for (auto& layer : layers)
        {
            if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM &&
                ((getGeTypeFromPrivateHandle(&layer->getPrivateHandle()) != GRALLOC_EXTRA_BIT_TYPE_CAMERA) &&
                (getGeTypeFromPrivateHandle(&layer->getPrivateHandle()) != GRALLOC_EXTRA_BIT_TYPE_VIDEO)))
            {
                layer->setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCD);
                --mm_layer_num;
            }
        }
    }

    // calculate extend mdp capacity for Primary display.
    uint32_t EXTEND_MAX_MM_NUM = (getId() == HWC_DISPLAY_PRIMARY) ?
        extendMDPCapacity(layers, mm_layer_num, m_num_camera_layer, m_num_video_layer) : 0;

    mm_layer_num = 0;
    for (auto& layer : layers)
    {
        // secure MM layer and non-secure MM layer with hint MM, never composited by client
        if ((layer->getHwlayerType() == HWC_LAYER_TYPE_MM &&
             (usageHasProtected(layer->getPrivateHandle().usage) ||
              layer->getPrivateHandle().sec_handle != 0)) ||
            (layer->getHwlayerType() == HWC_LAYER_TYPE_MM &&
             layer->isHint(HWC_LAYER_TYPE_MM)))
        {
            ++mm_layer_num;
        }
    }
    for (auto& layer : layers)
    {
        // non-secure MM layer without hint MM if excess, offload to client
        if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM &&
            !(usageHasProtected(layer->getPrivateHandle().usage) ||
              layer->getPrivateHandle().sec_handle != 0) &&
            (layer->getHwlayerType() != HWC_LAYER_TYPE_MM ||
             !layer->isHint(HWC_LAYER_TYPE_MM)))
        {
            if (mm_layer_num >= (MAX_MM_NUM + EXTEND_MAX_MM_NUM))
            {
                layer->setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCD);
            }
            else
            {
                ++mm_layer_num;
            }
        }
    }
}

inline static void fillHwLayer(
    const uint64_t& dpy, DispatcherJob* job, const sp<HWCLayer>& layer,
    const unsigned int& ovl_idx, const unsigned int& layer_idx, const int& ext_sel_layer)
{
    HWC_ATRACE_FORMAT_NAME("HWC(h:%p)", layer->getHandle());
    const PrivateHandle* priv_handle = &layer->getPrivateHandle();

    if (ovl_idx >= job->num_layers) {
        HWC_LOGE("try to fill HWLayer with invalid index: 0x%x(0x%x)", ovl_idx, job->num_layers);
        abort();
    }
    HWLayer* hw_layer  = &job->hw_layers[ovl_idx];
    hw_layer->enable   = true;
    hw_layer->index    = layer_idx;
    hw_layer->type     = layer->getHwlayerType();
    hw_layer->hwc_layer = layer;
    // hold a strong pointer on queue for this job life cycle
    hw_layer->queue = layer->getBufferQueue();

    hw_layer->dirty = HWCDispatcher::getInstance().decideDirtyAndFlush(dpy,
                                                                       ovl_idx,
                                                                       layer,
                                                                       job->hw_layers[ovl_idx]);
    hw_layer->hwc2_layer_id = layer->getId();

    if (layer->getHwlayerType() == HWC_LAYER_TYPE_MM)
    {
        hw_layer->mdp_output_compressed = layer->decideMdpOutputCompressedBuffers();
        hw_layer->mdp_output_format     = layer->decideMdpOutputFormat();

        hw_layer->mdp_dst_roi.left = layer->getMdpDstRoi().left;
        hw_layer->mdp_dst_roi.top = layer->getMdpDstRoi().top;
        hw_layer->mdp_dst_roi.right = layer->getMdpDstRoi().right;
        hw_layer->mdp_dst_roi.bottom = layer->getMdpDstRoi().bottom;

        // check mdp can skip invalidate
        if (hw_layer->dirty && hw_layer->dirty_reason == HW_LAYER_DIRTY_HWC_LAYER_STATE)
        {
            // dirty is only due to layer state change
            int state_change_reason = layer->getStateChangedReason();
            const int can_skip_reason = HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_OFFSET |
                                        HWC_LAYER_STATE_CHANGE_VISIBLE_REGION;    // not use in hwc
            if ((state_change_reason & ~can_skip_reason) == 0)
            {
                hw_layer->mdp_skip_invalidate = true;
            }
        }
    }

    if (layer->getHwlayerType() == HWC_LAYER_TYPE_GLAI)
    {
        hw_layer->glai_dst_roi.left = layer->getGlaiDstRoi().left;
        hw_layer->glai_dst_roi.top = layer->getGlaiDstRoi().top;
        hw_layer->glai_dst_roi.right = layer->getGlaiDstRoi().right;
        hw_layer->glai_dst_roi.bottom = layer->getGlaiDstRoi().bottom;
    }

    hw_layer->ext_sel_layer = Platform::getInstance().m_config.enable_smart_layer ? ext_sel_layer : -1;
    hw_layer->layer_caps    = layer->getLayerCaps();
    hw_layer->layer_color   = layer->getLayerColor();
    hw_layer->dataspace     = layer->getDataspace();
    hw_layer->need_pq       = layer->isNeedPQ(1);
    hw_layer->is_ai_pq      = layer->isAIPQ();
    hw_layer->game_hdr      = layer->isGameHDR();
    hw_layer->camera_preview_hdr = layer->isCameraPreviewHDR();
    hw_layer->glai_agent_id = layer->getGlaiAgentId();

    hw_layer->hdr_static_metadata_keys = layer->getHdrStaticMetadataKeys();
    hw_layer->hdr_static_metadata_values = layer->getHdrStaticMetadataValues();
    hw_layer->hdr_dynamic_metadata = layer->getHdrDynamicMetadata();

    memcpy(&hw_layer->priv_handle, priv_handle, sizeof(PrivateHandle));

    HWC_LOGV("(%" PRIu64 ") layer(%" PRIu64 ") hnd=%p caps=%d z=%d"
             "hw_layer[index=%u ovl_idx=%u enable=%d type=%d ion_fd=%d dirty=%d mdp_dst_roi=%d,%d,%d,%d mdp_fmt=%d mdp_comp=%d g_hdr=%d cp_hdr=%d hdr_meta=%zu,%zu,%zu]",
        dpy, layer->getId(), layer->getHandle(), layer->getLayerCaps(), layer->getZOrder(),
        hw_layer->index, ovl_idx, hw_layer->enable, hw_layer->type, hw_layer->priv_handle.ion_fd, hw_layer->dirty,
        layer->getMdpDstRoi().left, layer->getMdpDstRoi().top, layer->getMdpDstRoi().right, layer->getMdpDstRoi().bottom,
        hw_layer->mdp_output_format, hw_layer->mdp_output_compressed, hw_layer->game_hdr, hw_layer->camera_preview_hdr,
        hw_layer->hdr_static_metadata_keys.size(), hw_layer->hdr_static_metadata_values.size(), hw_layer->hdr_dynamic_metadata.size());
}

inline void setupHwcLayers(const sp<HWCDisplay>& display, DispatcherJob* job)
{
    HWC_LOGV("(%" PRIu64 ") + setupHwcLayers", display->getId());
    int32_t gles_head = -1, gles_tail = -1;
    const uint64_t disp_id = display->getId();

    display->getGlesRange(&gles_head, &gles_tail);

    job->num_ui_layers = 0;
    job->num_mm_layers = 0;
    job->num_glai_layers = 0;

    auto&& layers = display->getVisibleLayersSortedByZ();

    unsigned int ovl_index = 0;
    for (unsigned int i = 0; i < static_cast<unsigned int>(layers.size()); ++i)
    {
        sp<HWCLayer> layer = layers[i];
        const PrivateHandle* priv_handle = &layer->getPrivateHandle();

#ifdef MTK_HDR_SET_DISPLAY_COLOR
        // check if contains hdr layer
        job->is_HDR |= priv_handle->hdr_prop.is_hdr;
        job->is_HDR |= layer->getPerFrameMetadataBlobs().empty()?0:MTK_HDR10P_PROPERTY_FLAG;
        if (priv_handle->hdr_prop.is_hdr || !layer->getPerFrameMetadataBlobs().empty())
        {
            HWC_LOGD("(%" PRIu64 ")%s is_HDR_Gain i:%d layer id: %" PRIu64 " blobs size:%d "
                    "empty:%d, visable:%d, gain:%d",
                    display->getId(), __func__, i, layer->getId(),
                    static_cast<int>(layer->getPerFrameMetadataBlobs().size()),
                    layer->getPerFrameMetadataBlobs().empty(), layer->isVisible(), job->is_HDR);
        }
#endif

        if (layer->getHwlayerType() == HWC_LAYER_TYPE_INVALID)
            continue;

        const unsigned int ovl_id = job->layer_info.hrt_config_list[i].ovl_id;
        const int ext_sel_layer = job->layer_info.hrt_config_list[i].ext_sel_layer;

        HWC_LOGV("(%" PRIu64 ")   setupHwcLayers i:%u ovl_id:%u gles_head:%d, ovl_index:%u",
            display->getId(), i, ovl_id, gles_head, ovl_index);

        fillHwLayer(disp_id, job, layer, ovl_id, ovl_index, ext_sel_layer);

        // check if need to enable secure composition
        job->secure |= usageHasSecure(priv_handle->usage);
        const int32_t type = layer->getHwlayerType();
        switch (type)
        {
            case HWC_LAYER_TYPE_UI:
            case HWC_LAYER_TYPE_DIM:
            case HWC_LAYER_TYPE_CURSOR:
                ++job->num_ui_layers;
                break;

            case HWC_LAYER_TYPE_MM:
                ++job->num_mm_layers;
                break;

            case HWC_LAYER_TYPE_GLAI:
                ++job->num_glai_layers;
                break;
        }
        ovl_index++;
    }
    HWC_LOGV("(%" PRIu64 ") - setupHwcLayers", display->getId());
}

void HWCDisplay::calculatePerf(DispatcherJob* job)
{
    const HwcMCycleInfo& info = getScenarioMCInfo(job);
    job->mc_info = &info;

    if (Platform::getInstance().m_config.perf_prefer_below_cpu_mhz <= 0)
    {
        return;
    }

    // perf control for decouple mode, ex. MML DC / MDP / APU
    if (info.id >= HWC_MC_0U_1M && info.id <= HWC_MC_5U_1M)
    {
        float ovl_atomic_work_mc = info.ovl_mc * info.ovl_mc_atomic_ratio;
        nsecs_t ovl_atomic_work_time = static_cast<nsecs_t>((ovl_atomic_work_mc * 1e9) /
                                                            Platform::getInstance().m_config.perf_prefer_below_cpu_mhz);
        // although we count wait fence (in ovl) into Million Cylces, we calculate remain time after wait fence
        // lets reserve some time for wait fence to compensate this issue...
        ovl_atomic_work_time += job->num_ui_layers * us2ns(100);
        ovl_atomic_work_time += job->fbt_exist ? us2ns(100) : 0;

        ovl_atomic_work_time = std::max(ovl_atomic_work_time, static_cast<nsecs_t>(PMQOS_DISPLAY_DRIVER_EXECUTE_TIME));
        job->decouple_target_ts = job->sf_target_ts - ovl_atomic_work_time;
        HWC_ATRACE_INT64(m_perf_ovl_atomic_work_time_str.c_str(), static_cast<int64_t>(ovl_atomic_work_time));
    }
}

void HWCDisplay::setJobVideoTimeStamp(DispatcherJob* job)
{
    if (NULL == job)
        return;

    auto&& layers = getVisibleLayersSortedByZ();

    for (size_t i = 0; i < layers.size(); ++i)
    {
        sp<HWCLayer> layer = layers[i];
        if (layer->isClientTarget())
            continue;

        const PrivateHandle* priv_handle = &layer->getPrivateHandle();
        // check if need to set video timestamp
        unsigned int buffer_type = getGeTypeFromPrivateHandle(priv_handle);
        if (buffer_type == GRALLOC_EXTRA_BIT_TYPE_VIDEO)
            job->timestamp = priv_handle->ext_info.timestamp;
    }
}

void HWCDisplay::setGlesRange(const int32_t& gles_head, const int32_t& gles_tail)
{
    m_gles_head = gles_head;
    m_gles_tail = gles_tail;

    HWC_LOGV("setGlesRange() gles_head:%d gles_tail:%d", m_gles_head, m_gles_tail);
}

void HWCDisplay::updateGlesRangeHwType()
{
    if (m_gles_head == -1)
        return;

    auto&& layers = getVisibleLayersSortedByZ();
    HWC_LOGV("updateGlesRangeHwType() gles_head:%d gles_tail:%d", m_gles_head, m_gles_tail);

    m_client_clear_layer_num = 0;
    for (int32_t i = m_gles_head; i <= m_gles_tail; ++i)
    {
        auto& layer = layers[static_cast<unsigned int>(i)];
        if (layer->getHWCRequests() & HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET)
        {
            HWC_LOGV("setGlesRangeHwType() %d CLEAR_CLIENT", i);
            m_client_clear_layer_num++;
        }
        else
        {
            if (layer->getHwlayerType() != HWC_LAYER_TYPE_INVALID)
                layer->setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCD);
        }
    }
}

static void calculateFbtRoi(const sp<HWCDisplay>& display, const sp<HWCLayer>& fbt_layer)
{
    hwc_frect_t src_crop;
    src_crop.left = src_crop.top = 0;
    src_crop.right = fbt_layer->getPrivateHandle().width;
    src_crop.bottom = fbt_layer->getPrivateHandle().height;

    hwc_rect_t display_frame;
    display_frame.left = display_frame.top = 0;
    display_frame.right = static_cast<int>(fbt_layer->getPrivateHandle().width);
    display_frame.bottom = static_cast<int>(fbt_layer->getPrivateHandle().height);

    hwc_frect_t cut_src_crop;
    hwc_rect_t cut_display_frame;
    const hwc_rect_t disp_bound = {0, 0, display->getWidth(display->getActiveConfig()), display->getHeight(display->getActiveConfig())};
    if (boundaryCut(src_crop, display_frame, 0, disp_bound, &cut_src_crop, &cut_display_frame))
    {
        HWC_LOGD("(%" PRIu64 ") fbt crop xform=%d/Src(%.1f,%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f,%.1f)"
                 "/Dst(%d,%d,%d,%d)->(%d,%d,%d,%d)", display->getId(), 0,
                 src_crop.left, src_crop.top, src_crop.right, src_crop.bottom,
                 cut_src_crop.left, cut_src_crop.top, cut_src_crop.right, cut_src_crop.bottom,
                 display_frame.left, display_frame.top, display_frame.right, display_frame.bottom,
                 cut_display_frame.left, cut_display_frame.top, cut_display_frame.right, cut_display_frame.bottom);
    }
    fbt_layer->setSourceCrop(cut_src_crop);
    fbt_layer->setDisplayFrame(cut_display_frame);
    return;
}

static void setupGlesLayers(const sp<HWCDisplay>& display, DispatcherJob* job)
{
    int32_t gles_head = -1, gles_tail = -1;
    display->getGlesRange(&gles_head, &gles_tail);
    if (gles_head == -1)
    {
        job->fbt_exist = false;
        return ;
    }

    if (gles_head != -1 && display->getClientTarget()->getHandle() == nullptr)
    {
        // SurfaceFlinger might not setClientTarget while VDS disconnect.
        if (display->getId() == HWC_DISPLAY_PRIMARY)
        {
            HWC_LOGE("(%" PRIu64 ") %s: HWC does not receive client target's handle, g[%d,%d]", display->getId(), __func__, gles_head, gles_tail);
            job->fbt_exist = false;
            return ;
        }
        else
        {
            HWC_LOGW("(%" PRIu64 ") %s: HWC does not receive client target's handle, g[%d,%d] acq_fd:%d", display->getId(), __func__, gles_head, gles_tail, display->getClientTarget()->getAcquireFenceFd());
        }
    }

    job->fbt_exist = true;

    auto&& commit_layers = display->getCommittedLayers();

    const unsigned int ovl_id = job->layer_info.hrt_config_list[gles_head].ovl_id;
    const unsigned int fbt_hw_layer_idx = ovl_id;
    HWC_LOGV("setupGlesLayers() gles[%d,%d] fbt_hw_layer_idx:%u", gles_head, gles_tail, fbt_hw_layer_idx);

    sp<HWCLayer> fbt_layer = display->getClientTarget();

    // set roi of client target
    calculateFbtRoi(display, fbt_layer);

    if (fbt_hw_layer_idx >= job->num_layers) {
        HWC_LOGE("try to fill HWLayer with invalid index for client target: 0x%x(0x%x)", fbt_hw_layer_idx, job->num_layers);
        abort();
    }
    HWLayer* fbt_hw_layer = &job->hw_layers[fbt_hw_layer_idx];
    HWC_ATRACE_FORMAT_NAME("GLES CT(h:%p)", fbt_layer->getHandle());
    fbt_hw_layer->enable  = true;
    fbt_hw_layer->index   = static_cast<unsigned int>(commit_layers.size()) - 1;
    fbt_hw_layer->hwc2_layer_id = fbt_layer->getId();
    fbt_hw_layer->type = HWC_LAYER_TYPE_FBT;
    fbt_hw_layer->dirty = fbt_layer->isBufferChanged();
    fbt_hw_layer->ext_sel_layer = -1;
    fbt_hw_layer->dataspace    = fbt_layer->getDataspace();

    const PrivateHandle* priv_handle = &fbt_layer->getPrivateHandle();

#ifdef MTK_HDR_SET_DISPLAY_COLOR
    // check if contains hdr layer
    job->is_HDR |= priv_handle->hdr_prop.is_hdr;
    job->is_HDR |= fbt_layer->getPerFrameMetadataBlobs().empty()?0:MTK_HDR10P_PROPERTY_FLAG;
    if (priv_handle->hdr_prop.is_hdr || !fbt_layer->getPerFrameMetadataBlobs().empty())
    {
        HWC_LOGD("(%" PRIu64 ")%s is_HDR_Gain blobs size:%d empty:%d, visable:%d, gain:%d",
                display->getId(), __func__,
                static_cast<int>(fbt_layer->getPerFrameMetadataBlobs().size()),
                fbt_layer->getPerFrameMetadataBlobs().empty(), fbt_layer->isVisible(), job->is_HDR);
    }
#endif

    memcpy(&fbt_hw_layer->priv_handle, priv_handle, sizeof(PrivateHandle));
    if (Platform::getInstance().m_config.enable_smart_layer)
    {
        fbt_hw_layer->ext_sel_layer = job->layer_info.hrt_config_list[static_cast<unsigned int>(gles_head)].ext_sel_layer;
    }
}

void HWCDisplay::updateGlesRange()
{
    if (!isConnected())
        return;

    int32_t gles_head = -1, gles_tail = -1;
    if (HWCMediator::getInstance().getOvlDevice(getId())->getType() != OVL_DEVICE_TYPE_OVL &&
        getMirrorSrc() == -1 &&
        static_cast<int32_t>(getVisibleLayersSortedByZ().size() > 0))
    {
        gles_head = 0;
        gles_tail = static_cast<int32_t>(getVisibleLayersSortedByZ().size()) - 1;
    }
    else
    {
        findGlesRange(getVisibleLayersSortedByZ(), &gles_head, &gles_tail);
    }
    HWC_LOGV("updateGlesRange() gles_head:%d , gles_tail:%d", gles_head, gles_tail);
    setGlesRange(gles_head, gles_tail);

    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(getId());
    if (job != NULL)
    {
        job->layer_info.hwc_gles_head = gles_head;
        job->layer_info.hwc_gles_tail = gles_tail;
    }
}

void HWCDisplay::acceptChanges()
{
}

void HWCDisplay::setRetireFenceFd(const int32_t& retire_fence_fd, const bool& is_disp_connected)
{
    if (retire_fence_fd >= 0 && m_retire_fence_fd != -1)
    {
        if (is_disp_connected)
        {
            HWC_LOGE("(%" PRIu64 ") fdleak detect: %s retire_fence_fd:%d",
                getId(), __func__, m_retire_fence_fd);
            AbortMessager::getInstance().abort();
        }
        else
        {
            HWC_LOGW("(%" PRIu64 ") fdleak detect: %s retire_fence_fd:%d",
                getId(), __func__, m_retire_fence_fd);
            ::protectedClose(m_retire_fence_fd);
            m_retire_fence_fd = -1;
        }
    }
    m_retire_fence_fd = retire_fence_fd;
}

void HWCDisplay::setColorTransformForJob(DispatcherJob* const job)
{
    CCORR_STATE old_ccorr_state = m_ccorr_state;
    // ccorr state transition
    const bool skip_client_color_transform =
        Platform::getInstance().m_config.check_skip_client_color_transform &&
        HWCMediator::getInstance().getOvlDevice(HWC_DISPLAY_PRIMARY)->isDisp3X4DisplayColorTransformSupported();

    const bool fbt_only =
        !skip_client_color_transform &&
        job->fbt_exist && (static_cast<uint32_t>(job->num_ui_layers + job->num_mm_layers + job->num_glai_layers) + getInvisibleLayersSortedByZ().size() == 0);
    if (fbt_only)
    {
        switch (m_ccorr_state)
        {
            case FIRST_FBT_ONLY:
                m_ccorr_state = STILL_FBT_ONLY;
                break;

            case NOT_FBT_ONLY:
                m_ccorr_state = FIRST_FBT_ONLY;
                break;

            default:
                break;
        }
    }
    else
    {
        m_ccorr_state = NOT_FBT_ONLY;
    }

    // ccorr state action
    job->color_transform = nullptr;
    bool do_set_color_transform = false;
    switch (m_ccorr_state)
    {
        case FIRST_FBT_ONLY:
            {
                bool is_identity = false;
                if (m_color_transform != nullptr)
                {
                    is_identity = m_color_transform->isIdentity();
                }
                if (is_identity)
                {
                    // Reuse non-dirty identity color transform
                    if (m_color_transform->dirty)
                    {
                        do_set_color_transform = true;
                    }
                    else
                    {
                        job->color_transform = nullptr;
                    }
                }
                else
                {
                    sp<ColorTransform> color = new ColorTransform(HAL_COLOR_TRANSFORM_IDENTITY, true, true);
                    job->color_transform = color;
                    // We need to set color transform back when enter NOT_FBT_ONLY state
                    if (m_color_transform != nullptr)
                    {
                        m_color_transform->dirty = true;
                    }
                }
            }
            break;

        case STILL_FBT_ONLY:
            job->color_transform = nullptr;
            break;

        case NOT_FBT_ONLY:
            if (m_color_transform != nullptr && m_color_transform->dirty)
                do_set_color_transform = true;
            break;
    }
    if (do_set_color_transform)
    {
        sp<ColorTransform> color = new ColorTransform(
            m_color_transform->matrix,
            m_color_transform->hint,
            true,
            false);
        m_color_transform->dirty = false;
        job->color_transform = color;
    }
    if (job->color_transform != nullptr)
    {
        HWC_LOGI("%s ccorr_state=%d->%d hint=%d force_disable_color=%d identity=%d", __func__, old_ccorr_state, m_ccorr_state,
            job->color_transform->hint, job->color_transform->force_disable_color, job->color_transform->isIdentity());
    }
}

void HWCDisplay::beforePresent(const unsigned int num_plugin_display)
{
    if (getId() == HWC_DISPLAY_VIRTUAL)
    {
        if (getOutbuf() == nullptr)
        {
            HWC_LOGE("(%" PRIu64 ") outbuf missing", getId());
            clearAllFences();
            return;
        }
        else if (getOutbuf()->getHandle() == nullptr)
        {
            HWC_LOGE("(%" PRIu64 ") handle of outbuf missing", getId());
            clearAllFences();
            return;
        }
    }

    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(getId());
    if (NULL != job)
    {
        getGlesRange(&job->layer_info.gles_head, &job->layer_info.gles_tail);

        if (getMirrorSrc() != -1)
        {
            // prepare job in job group
            job->fbt_exist     = false;
            job->num_ui_layers = 0;
            // [NOTE] treat mirror source as mm layer
            job->num_mm_layers = 1;
            job->hw_layers[0].enable = true;
            job->hw_layers[0].type = HWC_LAYER_TYPE_MM;
        }
        else
        {
            setupHwcLayers(this, job);
            setupGlesLayers(this, job);
            if (Platform::getInstance().m_config.plat_switch &
                    (HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_VIDEO |
                    HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_CAMERA))
            {
                extendSfTargetTime(job);
            }
            calculatePerf(job);

            job->post_state     = HWC_POST_INPUT_DIRTY;
        }

        HWCDispatcher::getInstance().fillPrevHwLayers(this, job);

        setColorTransformForJob(job);

        if (job->layer_info.max_overlap_layer_num == -1)
        {
            job->layer_info.max_overlap_layer_num = job->num_ui_layers
                                                  + job->num_mm_layers
                                                  + job->num_glai_layers
                                                  + (job->fbt_exist ? 1 : 0);
        }

        job->is_full_invalidate =
            HWCMediator::getInstance().getOvlDevice(getId())->isPartialUpdateSupported() ? isGeometryChanged() : true;

        if (needDoAvGrouping(num_plugin_display))
            job->need_av_grouping = true;


        if (HwcFeatureList::getInstance().getFeature().game_pq >= 2 && getId()== HWC_DISPLAY_PRIMARY)
        {
            auto&& commit_layers = getCommittedLayers();
            bool app_game_pq_on = false;
            for (auto& layer : commit_layers)
            {
                if ((!layer->isClientTarget() && layer->isNeedPQ(2)) ||
                    (layer->isClientTarget() && layer->isNeedPQ(2) && job->fbt_exist))
                {
                    if (!getLastAppGamePQ())
                    {
                        HWC_LOGI("enable AppGamePQ2");
                    }
                    getPqDevice()->setGamePQHandle(layer->getHandle());
                    app_game_pq_on = true;
                    break;
                }
            }
            if (getLastAppGamePQ() && !app_game_pq_on)
            {
                HWC_LOGI("disable AppGamePQ2");
                getPqDevice()->setGamePQHandle(nullptr);
            }
            setLastAppGamePQ(app_game_pq_on);
        }
        job->dirty_pq_mode_id = (m_pq_mode_id == m_presented_pq_mode_id) ? false : true;
        job->pq_mode_id = m_pq_mode_id;
        m_presented_pq_mode_id = m_pq_mode_id;

        HWC_LOGD("(%" PRIu64 ") VAL list=%zu/max=%u/fbt=%d[%d,%d:%d,%d]/hrt=%d,%d/ui=%d/mm=%d/glai=%d/"
                    "inv=%zu/ovlp=%d/fi=%d/mir=%d/pq_mode_id=%d",
                getId(), getVisibleLayersSortedByZ().size(), job->num_layers,
                job->fbt_exist,job->layer_info.hwc_gles_head, job->layer_info.hwc_gles_tail,
                job->layer_info.gles_head, job->layer_info.gles_tail,
                job->layer_info.hrt_weight, job->layer_info.hrt_idx,
                job->num_ui_layers, job->num_mm_layers, job->num_glai_layers,
                getInvisibleLayersSortedByZ().size(),
                job->layer_info.max_overlap_layer_num, job->is_full_invalidate,
                job->disp_mir_id, job->pq_mode_id);

        setPrevAvailableInputLayerNum(job->num_layers);
    }
    else
    {
        clearAllFences();
        if (m_dispathcer_job_status != HWC_DISPACHERJOB_INVALID_DROPJOB)
        {
            HWC_LOGE("(%" PRIu64 ") job is null(%d)", getId(), m_dispathcer_job_status);
        }
        else
        {
            HWC_LOGV("(%" PRIu64 ") job is null(%d)", getId(), m_dispathcer_job_status);
        }
    }
}

void HWCDisplay::present()
{
    setValiPresentState(HWC_VALI_PRESENT_STATE_PRESENT, __LINE__);

    if (getId() == HWC_DISPLAY_VIRTUAL)
    {
        if (getOutbuf() == nullptr)
        {
            HWC_LOGE("(%" PRIu64 ") outbuf missing", getId());
            return;
        }
        else if (getOutbuf()->getHandle() == nullptr)
        {
            HWC_LOGE("(%" PRIu64 ") handle of outbuf missing", getId());
            return;
        }
    }

    updateFps();
    HWCDispatcher::getInstance().setJob(this);
}

void HWCDisplay::afterPresent()
{
    setLastCommittedLayers(getCommittedLayers());
    setMirrorSrc(-1);

    bool should_clear_state = !m_dispathcer_job_status;
    for (auto& kv : m_layers)
    {
        auto& layer = kv.second;
        layer->afterPresent(should_clear_state);
    }

    if (getOutbuf() != nullptr)
    {
        getOutbuf()->afterPresent();
        if (getOutbuf()->getReleaseFenceFd() != -1)
        {
            if (isConnected())
            {
                HWC_LOGE("(%" PRIu64 ") %s getReleaseFenceFd:%d", getId(), __func__, getOutbuf()->getReleaseFenceFd());
                AbortMessager::getInstance().abort();
            }
            else
            {
                HWC_LOGW("(%" PRIu64 ") %s getReleaseFenceFd:%d", getId(), __func__, getOutbuf()->getReleaseFenceFd());
                ::protectedClose(getOutbuf()->getReleaseFenceFd());
                getOutbuf()->setReleaseFenceFd(-1, isConnected());
            }
        }
    }

    // just set as -1, do not close!!!
    if (getRetireFenceFd() > -1)
    {
        if (isConnected())
        {
            HWC_LOGE("(%" PRIu64 ") %s getRetireFenceFd():%d", getId(), __func__, getRetireFenceFd());
            AbortMessager::getInstance().abort();
        }
        else
        {

            HWC_LOGW("(%" PRIu64 ") %s getRetireFenceFd():%d", getId(), __func__, getRetireFenceFd());
            ::protectedClose(getRetireFenceFd());
            setRetireFenceFd(-1, isConnected());
        }
    }

    decUnpresentCount();

    if (CC_UNLIKELY(Platform::getInstance().m_config.dbg_present_delay_time != std::chrono::microseconds::zero())) {
        HWC_LOGI("slow motion timer: %" PRId64 " us",
                 static_cast<int64_t>(Platform::getInstance().m_config.dbg_present_delay_time.count()));
        ATRACE_NAME((std::string("slow motion timer: ") +
                     std::to_string(Platform::getInstance().m_config.dbg_present_delay_time.count())).c_str());
        std::this_thread::sleep_for(Platform::getInstance().m_config.dbg_present_delay_time);
    }
}

void HWCDisplay::clear()
{
    m_outbuf = nullptr;
    m_changed_comp_types.clear();
    m_layers.clear();
    m_ct = nullptr;
    m_prev_comp_types.clear();
    m_pending_removed_layers_id.clear();
    m_unpresent_count = 0;
    m_prev_unpresent_count = 0;
    m_changed_hwc_requests.clear();
    m_dispathcer_job_status = HWC_DISPACHERJOB_VALID;
    m_is_m4u_sec_inited = false;
    m_last_committed_layers.clear();
    m_committed_layers.clear();
    m_visible_layers.clear();
    m_invisible_layers.clear();
}

bool HWCDisplay::isConnected() const
{
    return DisplayManager::getInstance().getDisplayData(m_disp_id)->connected;
}

bool HWCDisplay::isValidated() const
{
    return m_is_validated;
}

void HWCDisplay::removePendingRemovedLayers()
{
    AutoMutex l(m_pending_removed_layers_mutex);
    for (auto& layer_id : m_pending_removed_layers_id)
    {
        if (m_layers.find(layer_id) != m_layers.end())
        {
            auto& layer = m_layers[layer_id];
            if (layer->isVisible())
            {
                HWC_LOGE("(%" PRIu64 ") false removed layer %s", getId(), layer->toString8().string());
            }
            else
            {
#ifdef MTK_USER_BUILD
                HWC_LOGV("(%" PRIu64 ") %s: destroy layer id(%" PRIu64 ")", getId(), __func__, layer_id);
#else
                HWC_LOGD("(%" PRIu64 ") %s: destroy layer id(%" PRIu64 ")", getId(), __func__, layer_id);
#endif
            }
            layer = nullptr;
            m_layers.erase(layer_id);
        }
        else
        {
            HWC_LOGE("(%" PRIu64 ") %s: destroy layer id(%" PRIu64 ") failed", getId(), __func__, layer_id);
        }
    }
    m_pending_removed_layers_id.clear();
}

void HWCDisplay::getChangedCompositionTypes(
    uint32_t* out_num_elem,
    hwc2_layer_t* out_layers,
    int32_t* out_types) const
{
    if (out_num_elem != NULL)
        *out_num_elem = static_cast<uint32_t>(m_changed_comp_types.size());

    if (out_layers != NULL)
        for (size_t i = 0; i < m_changed_comp_types.size(); ++i)
            out_layers[i] = m_changed_comp_types[i]->getId();

    if (out_types != NULL)
    {
        for (size_t i = 0; i < m_changed_comp_types.size(); ++i)
        {
            out_types[i] = m_changed_comp_types[i]->getCompositionType();
        }
    }
}

void HWCDisplay::getCompositionMode(bool& has_changed, int& new_mode)
{
    AutoMutex lock(m_dump_lock);
    new_mode = MTK_COMPOSITION_MODE_NORMAL;

    for (auto iter = m_visible_layers.begin(); iter != m_visible_layers.end(); iter++)
    {
        auto& layer = (*iter);

        int32_t hw_layer_type = layer->getHwlayerType();

        if (hw_layer_type == HWC_LAYER_TYPE_GLAI)
        {
            new_mode = MTK_COMPOSITION_MODE_DECOUPLE;
            break;
        }

        if (hw_layer_type == HWC_LAYER_TYPE_MM)
        {
            int32_t caps = layer->getLayerCaps();

            if ((caps & HWC_MML_DISP_DIRECT_DECOUPLE_LAYER) != 0 ||
                (caps & HWC_MML_DISP_DIRECT_LINK_LAYER) != 0)
            {
                continue;
            }

            new_mode = MTK_COMPOSITION_MODE_DECOUPLE;
            break;
        }
    }

    has_changed = m_prev_composition_mode != new_mode;
    m_prev_composition_mode = new_mode;
}

void HWCDisplay::getChangedRequests(
    uint32_t* out_num_elem,
    hwc2_layer_t* out_layers,
    int32_t* out_requests) const
{
    if (out_num_elem != NULL)
        *out_num_elem = static_cast<uint32_t>(m_changed_hwc_requests.size());

    if (out_layers != NULL)
        for (size_t i = 0; i < m_changed_hwc_requests.size(); ++i)
        {
            out_layers[i] = m_changed_hwc_requests[i]->getId();
        }

    if (out_requests != NULL)
    {
        for (size_t i = 0; i < m_changed_hwc_requests.size(); ++i)
        {
            out_requests[i] = m_changed_hwc_requests[i]->getHWCRequests();
            HWC_LOGV("getChangedRequests %" PRIu64" %d", m_changed_hwc_requests[i]->getId(), out_requests[i]);
        }
    }
}

sp<HWCLayer> HWCDisplay::getLayer(const hwc2_layer_t& layer_id)
{
    const auto& iter = m_layers.find(layer_id);
    if (iter == m_layers.end())
    {
        HWC_LOGE("(%" PRIu64 ") %s %" PRIu64, getId(), __func__, layer_id);
        for (auto& kv : m_layers)
        {
            auto& layer = kv.second;
            HWC_LOGE("(%" PRIu64 ") getLayer() %s", getId(), layer->toString8().string());
        }
        if (HWC_DISPLAY_EXTERNAL == getId())
        {
            HWC_LOGE("warning!!! external display getLayer failed!");
        }
        else
        {
            abort();
        }
    }
    return (iter == m_layers.end()) ? nullptr : iter->second;
}

void HWCDisplay::checkVisibleLayerChange(const std::vector<sp<HWCLayer> > &prev_visible_layers)
{
    m_is_visible_layer_changed = false;
    if (m_visible_layers.size() != prev_visible_layers.size())
    {
        m_is_visible_layer_changed = true;
    }
    else
    {
        for(size_t i = 0; i < prev_visible_layers.size(); i++)
        {
            if (prev_visible_layers[i]->getId() != m_visible_layers[i]->getId())
            {
                m_is_visible_layer_changed = true;
                break;
            }
        }
    }

    if (isVisibleLayerChanged())
    {
        for (auto& layer : getVisibleLayersSortedByZ())
        {
            // need restore private handle format, if force rgbx layer is not bottom layer
            if ((layer->getMtkFlags() & HWC_LAYER_FLAG_FORCE_RGBX) &&
                layer->getId() != m_visible_layers[0]->getId())
            {
                layer->setStateChanged(HWC_LAYER_STATE_CHANGE_FORCE_RGBX);
                layer->setMtkFlags(layer->getMtkFlags() & ~HWC_LAYER_FLAG_FORCE_RGBX);
            }
        }
    }
}

void HWCDisplay::buildVisibleAndInvisibleLayersSortedByZ()
{
    AutoMutex lock(m_dump_lock);
    const std::vector<sp<HWCLayer> > prev_visible_layers(m_visible_layers);
    m_visible_layers.clear();
    {
        AutoMutex l(m_pending_removed_layers_mutex);
        for(auto &kv : m_layers)
        {
            auto& layer = kv.second;
            if (m_pending_removed_layers_id.find(kv.first) == m_pending_removed_layers_id.end() &&
                !layer->isClientTarget())
            {
                m_visible_layers.push_back(kv.second);
            }
        }
    }

    sort(m_visible_layers.begin(), m_visible_layers.end(),
        [](const sp<HWCLayer>& a, const sp<HWCLayer>& b)
        {
            return a->getZOrder() < b->getZOrder();
        });

    m_invisible_layers.clear();
    if (m_visible_layers.size() > 1 &&
        Platform::getInstance().m_config.remove_invisible_layers && m_color_transform_ok &&
        !m_use_gpu_composition)
    {
        const uint32_t black_mask = 0x0;
        for (auto iter = m_visible_layers.begin(); iter != m_visible_layers.end();)
        {
            auto& layer = (*iter);
            if (layer->getSFCompositionType() == HWC2_COMPOSITION_SOLID_COLOR &&
                (((layer->getLayerColor() << 8) >> 8) | black_mask) == 0U)
            {
                m_invisible_layers.push_back(layer);
                m_visible_layers.erase(iter);
                continue;
            }
            break;
        }
    }

    // move layer to m_invisible_layers via hint if possible
    if (Platform::getInstance().m_config.hint_id > 0 &&
        Platform::getInstance().m_config.hint_hwlayer_type == HWC_LAYER_TYPE_IGNORE)
    {
        for (auto iter = m_visible_layers.begin(); iter != m_visible_layers.end(); iter++)
        {
            auto& layer = (*iter);
            if (layer->isHint(HWC_LAYER_TYPE_IGNORE))
            {
                layer->setHwlayerType(HWC_LAYER_TYPE_IGNORE, __LINE__, HWC_COMP_FILE_HWCD);
                m_invisible_layers.push_back(layer);
                m_visible_layers.erase(iter);
                break;
            }
        }
    }

    checkVisibleLayerChange(prev_visible_layers);

    if (isVisibleLayerChanged())
    {
        m_need_free_fb_cache = true;
    }
}

const std::vector<sp<HWCLayer> >& HWCDisplay::getVisibleLayersSortedByZ()
{
    return m_visible_layers;
}

const std::vector<sp<HWCLayer> >& HWCDisplay::getInvisibleLayersSortedByZ()
{
    return m_invisible_layers;
}

void HWCDisplay::buildCommittedLayers()
{
    AutoMutex lock(m_dump_lock);
    auto& visible_layers = getVisibleLayersSortedByZ();
    m_committed_layers.clear();
    for(auto &layer : visible_layers)
    {
        auto& f = layer->getDisplayFrame();
        HWC_LOGV("(%" PRIu64 ")  getCommittedLayers() i:%" PRIu64 " f[%d,%d,%d,%d] is_ct:%d comp:%s, %s",
            getId(),
            layer->getId(),
            f.left,
            f.top,
            f.right,
            f.bottom,
            layer->isClientTarget(),
            getCompString(layer->getCompositionType()),
            getHWLayerString(layer->getHwlayerType()));
        if (f.left != f.right && f.top != f.bottom &&
            !layer->isClientTarget() &&
            (layer->getCompositionType() == HWC2_COMPOSITION_DEVICE ||
             layer->getCompositionType() == HWC2_COMPOSITION_CURSOR) &&
            layer->getHwlayerType() != HWC_LAYER_TYPE_WORMHOLE)
        {
            HWC_LOGV("(%" PRIu64 ")  getCommittedLayers() i:%" PRIu64 " added",
                getId(), layer->getId());
            m_committed_layers.push_back(layer);
        }
    }

    sp<HWCLayer> ct = getClientTarget();
    HWC_LOGV("(%" PRIu64 ")  getCommittedLayers() ct handle:%p",
        getId(), ct->getHandle());
    if (ct->getHandle() != nullptr)
        m_committed_layers.push_back(ct);
}

const std::vector<sp<HWCLayer> >& HWCDisplay::getCommittedLayers()
{
    return m_committed_layers;
}

sp<HWCLayer> HWCDisplay::getClientTarget()
{
    if (m_layers.size() < 1)
    {
        HWC_LOGE("%s: there is no client target layer at display(%" PRIu64 ")", __func__, m_disp_id);
        return nullptr;
    }
    return m_ct;
}

int32_t HWCDisplay::getWidth(hwc2_config_t config) const
{
    return DisplayManager::getInstance().getDisplayData(m_disp_id, config)->width;
}

int32_t HWCDisplay::getHeight(hwc2_config_t config) const
{
    return DisplayManager::getInstance().getDisplayData(m_disp_id, config)->height;
}

nsecs_t HWCDisplay::getVsyncPeriod(hwc2_config_t config) const
{
    return DisplayManager::getInstance().getDisplayData(m_disp_id, config)->refresh;
}

int32_t HWCDisplay::getDpiX(hwc2_config_t config) const
{
    return static_cast<int32_t>(DisplayManager::getInstance().getDisplayData(m_disp_id, config)->xdpi);
}

int32_t HWCDisplay::getDpiY(hwc2_config_t config) const
{
    return static_cast<int32_t>(DisplayManager::getInstance().getDisplayData(m_disp_id, config)->ydpi);
}

int32_t HWCDisplay::getConfigGroup(hwc2_config_t config) const
{
    return static_cast<int32_t>(DisplayManager::getInstance().getDisplayData(m_disp_id, config)->group);
}

uint32_t HWCDisplay::getNumConfigs() const
{
    return HWCMediator::getInstance().getOvlDevice(m_disp_id)->getNumConfigs(m_disp_id);
}

bool HWCDisplay::setActiveConfig(hwc2_config_t config)
{
    if (m_disp_id == HWC_DISPLAY_PRIMARY &&
        config < getNumConfigs() &&
        m_active_config != config)
    {
        if (DisplayManager::getInstance().getDisplayData(m_disp_id, config)->refresh !=
            DisplayManager::getInstance().getDisplayData(m_disp_id, m_active_config)->refresh)
        {
            HWVSyncEstimator::getInstance().resetAvgVSyncPeriod(
                DisplayManager::getInstance().getDisplayData(m_disp_id, config)->refresh);
        }
        m_active_config = config;
        m_config_changed = true;

        return true;
    }
    HWC_LOGE("(%" PRIu64 ") set active config(%d) failure", m_disp_id, config);
    return false;
}

bool HWCDisplay::isConfigChanged()
{
    bool config_changed = m_config_changed;
    m_config_changed = false;
    return config_changed;
}

int32_t HWCDisplay::getSecure() const
{
    return DisplayManager::getInstance().getDisplayData(m_disp_id)->secure;
}

void HWCDisplay::setPowerMode(const int32_t& mode)
{
    // screen blanking based on early_suspend in the kernel
    HWC_LOGI("Display(%" PRIu64 ") SetPowerMode(%d)", m_disp_id, mode);
    m_power_mode = mode;
    DisplayManager::getInstance().setDisplayPowerState(m_disp_id, mode);

    if (m_disp_id == HWC_DISPLAY_EXTERNAL && mode == HWC2_POWER_MODE_OFF)
    {
        DisplayManager::getInstance().requestVSync(m_disp_id, false);
    }

    HWCDispatcher::getInstance().setPowerMode(m_disp_id, mode);

    if (m_disp_id == HWC_DISPLAY_EXTERNAL && mode == HWC2_POWER_MODE_ON)
    {
        DisplayManager::getInstance().requestVSync(m_disp_id, true);
    }

    DisplayManager::getInstance().setPowerMode(m_disp_id, mode);

    // disable mirror mode when display blanks
    if ((Platform::getInstance().m_config.mirror_state & MIRROR_DISABLED) != MIRROR_DISABLED)
    {
        if (mode == HWC2_POWER_MODE_OFF || HWC2_POWER_MODE_DOZE_SUSPEND == mode)
        {
            Platform::getInstance().m_config.mirror_state |= MIRROR_PAUSED;
        }
        else
        {
            Platform::getInstance().m_config.mirror_state &= ~MIRROR_PAUSED;
        }
    }
}

void HWCDisplay::setVsyncEnabled(const int32_t& enabled)
{
    DisplayManager::getInstance().requestVSync(m_disp_id, enabled);
}

void HWCDisplay::getType(int32_t* out_type) const
{
    *out_type = m_type;
}

int32_t HWCDisplay::createLayer(hwc2_layer_t* out_layer, const bool& is_ct)
{
    sp<HWCLayer> layer = new HWCLayer(this, getId(), is_ct);
    if(layer == nullptr)
    {
        HWC_LOGE("%s: Fail to alloc a layer", __func__);
        return HWC2_ERROR_NO_RESOURCES;
    }

    m_layers[layer->getId()] = layer;
    *out_layer = layer->getId();

    if (is_ct)
    {
        layer->setPlaneAlpha(1.0f);
        layer->setBlend(HWC2_BLEND_MODE_PREMULTIPLIED);
        layer->setHwlayerType(HWC_LAYER_TYPE_FBT, __LINE__, HWC_COMP_FILE_HWCD);
    }
    HWC_LOGD("(%" PRIu64 ") %s out_layer:%" PRIu64, getId(), __func__, *out_layer);
    return HWC2_ERROR_NONE;
}

int32_t HWCDisplay::destroyLayer(const hwc2_layer_t& layer_id)
{
    HWC_LOGD("(%" PRIu64 ") %s layer:%" PRIu64, getId(), __func__, layer_id);

    AutoMutex l(m_pending_removed_layers_mutex);
    std::pair<std::set<uint64_t>::iterator, bool> ret = m_pending_removed_layers_id.insert(layer_id);
    if (ret.second == false)
    {
        HWC_LOGE("(%" PRIu64 ") To destroy layer id(%" PRIu64 ") twice", getId(), layer_id);
    }
    return HWC2_ERROR_NONE;
}

void HWCDisplay::clearAllFences()
{
    const int32_t retire_fence_fd = getRetireFenceFd();

    // if copyvds is false, if composition type is GLES only
    // HWC must take the acquire fence of client target as retire fence,
    // so retire fence cannot be closed.
    int32_t gles_head = -1, gles_tail = -1;
    getGlesRange(&gles_head, &gles_tail);
    if (retire_fence_fd != -1 &&
        !(getId() == HWC_DISPLAY_VIRTUAL && gles_head == 0 && gles_tail == static_cast<int32_t>(getVisibleLayersSortedByZ().size() - 1) &&
          !HwcFeatureList::getInstance().getFeature().copyvds))
    {
        protectedClose(retire_fence_fd);
        setRetireFenceFd(-1, isConnected());
    }

    if (getOutbuf() != nullptr)
    {
        const int32_t outbuf_release_fence_fd = getOutbuf()->getReleaseFenceFd();
        if (outbuf_release_fence_fd != -1)
        {
            protectedClose(outbuf_release_fence_fd);
            getOutbuf()->setReleaseFenceFd(-1, isConnected());
        }
    }

    for (auto& kv : m_layers)
    {
        auto& layer = kv.second;
        const int32_t release_fence_fd = layer->getReleaseFenceFd();
        if (release_fence_fd != -1)
        {
            protectedClose(release_fence_fd);
            layer->setReleaseFenceFd(-1, isConnected());
        }
    }
}

void HWCDisplay::getReleaseFenceFds(
    uint32_t* out_num_elem,
    hwc2_layer_t* out_layer,
    int32_t* out_fence_fd)
{
    static bool flip = 0;

    if (!flip)
    {
        *out_num_elem = 0;
        for (auto& kv : m_layers)
        {
            auto& layer = kv.second;

            if (layer->isClientTarget())
                continue;

            if (layer->getPrevReleaseFenceFd() != -1)
                ++(*out_num_elem);
        }
    }
    else
    {
        int32_t out_fence_fd_cnt = 0;
        for (auto& kv : m_layers)
        {
            auto& layer = kv.second;

            if (layer->isClientTarget())
                continue;

            if (layer->getPrevReleaseFenceFd() != -1)
            {
                out_layer[out_fence_fd_cnt] = layer->getId();
                const int32_t prev_rel_fd = layer->getPrevReleaseFenceFd();
#ifdef USES_FENCE_RENAME
                const int32_t hwc_to_sf_rel_fd = SyncFence::merge(prev_rel_fd, prev_rel_fd, "HWC_to_SF_rel");
                out_fence_fd[out_fence_fd_cnt] = hwc_to_sf_rel_fd;
#else
                out_fence_fd[out_fence_fd_cnt] = ::dup(prev_rel_fd);
#endif
                ::protectedClose(prev_rel_fd);
                layer->setPrevReleaseFenceFd(-1, isConnected());
                // just set release fence fd as -1, and do not close it!!!
                // release fences cannot close here
                ++out_fence_fd_cnt;
            }
            layer->setPrevReleaseFenceFd(layer->getReleaseFenceFd(), isConnected());
            layer->setReleaseFenceFd(-1, isConnected());
        }
        //{
        //    DbgLogger logger(DbgLogger::TYPE_HWC_LOG, 'D',"(%d) hwc2::getReleaseFenceFds() out_num_elem:%d", getId(), *out_num_elem);
        //    for (int i = 0; i < *out_num_elem; ++i)
        //        logger.printf("(layer id:%d fence fd:%d) ",
        //                out_layer[i],
        //                out_fence_fd[i]);
        //}
    }
    flip = !flip;
}

void HWCDisplay::getClientTargetSupport(
    const uint32_t& /*width*/, const uint32_t& /*height*/,
    const int32_t& /*format*/, const int32_t& /*dataspace*/)
{
    /*
    auto& layer = getClientTarget();
    if (layer != nullptr)
        layer->setFormat
    */
}

void HWCDisplay::setOutbuf(const buffer_handle_t& handle, const int32_t& release_fence_fd)
{
    HWC_LOGV("(%" PRIu64 ") HWCDisplay::setOutbuf() handle:%p release_fence_fd:%d m_outbuf:%p", getId(), handle, release_fence_fd, m_outbuf.get());
    m_outbuf->setHandle(handle);
    m_outbuf->setupPrivateHandle(nullptr);
    m_outbuf->setReleaseFenceFd(release_fence_fd, isConnected());
    if (handle != nullptr && getId() == HWC_DISPLAY_VIRTUAL)
    {
        PrivateHandle priv_handle = getOutbuf()->getPrivateHandle();
        bool is_secure = usageHasProtectedOrSecure(priv_handle.usage);
        uint64_t alloc_id = priv_handle.alloc_id;
        if (!m_is_m4u_sec_inited && is_secure)
        {
            initSecureM4U();
            m_is_m4u_sec_inited = true;
        }
        HWC_LOGD("%s handle:%p, alloc_id:%" PRIu64 ", sec_handle=0x%x, %p", __func__,
                handle, alloc_id, priv_handle.sec_handle, (void*)(uintptr_t)priv_handle.sec_handle);
    }
}

void HWCDisplay::dump(String8* dump_str)
{
    m_dump_lock.lock();
    // copy the variables which might changed by main thread
    // since dump is called by another thread.
    const std::vector<sp<HWCLayer> > copy_visible_layers(m_visible_layers);
    const std::vector<sp<HWCLayer> > copy_invisible_layers(m_invisible_layers);
    const std::vector<sp<HWCLayer> > copy_committed_layers(m_committed_layers);
    const int32_t color_mode = m_color_mode;
    const int32_t render_intent = m_render_intent;
    m_dump_lock.unlock();

    dump_str->appendFormat("Display(%" PRIu64 ")\n", getId());
    dump_str->appendFormat(" visible_layers:%zu invisible_layers:%zu commit_layers:%zu",
        copy_visible_layers.size(),
        copy_invisible_layers.size(),
        copy_committed_layers.size());

    if (getClientTarget()->getHandle())
    {
#ifdef MTK_IN_DISPLAY_FINGERPRINT
        dump_str->appendFormat(" ct_handle:%p ct_fbdc:%d",
#else
        dump_str->appendFormat(" ct_handle:%p ct_fbdc:%d\n",
#endif
            getClientTarget()->getHandle(),
            isCompressData(&(getClientTarget()->getPrivateHandle())));
    }
#ifndef MTK_IN_DISPLAY_FINGERPRINT
    else
    {
        dump_str->appendFormat("\n");
    }
#else
    dump_str->appendFormat(" hbm:%d", getIsHBM());
    dump_str->appendFormat("\n");
#endif
    dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");
    dump_str->appendFormat("| layer id |             handle / alloc_id |      fmt | blend |                                 comp | tr|         ds |c|        pq   |      usage | gralloc range  |   caps |\n");

    for (auto& layer : copy_visible_layers)
    {
        dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");
        dump_str->appendFormat("|%9" PRId64 " | %18p /%9" PRId64 " | %8x | %5s | %3s(%8s,%3s,%3s,%5s:%5d) %3x| %2u| %10d |%d|%d,%3x,%d,%d,%d,%d|%12x|%16x|%8x|\n",
            layer->getId(),
            layer->getHandle(),
            layer->getPrivateHandle().alloc_id,
            layer->getPrivateHandle().format,
            getBlendString(layer->getBlend()),
            getCompString(layer->getCompositionType()),
            getHWLayerString(layer->getHwlayerType(), layer->getLayerCaps()),
            getCompString(layer->getSFCompositionType()),
            getCompString(layer->getLastCompTypeCallFromSF()),
            getCompFileString(layer->getHwlayerTypeFile()),
            layer->getHwlayerTypeLine(),
            layer->getHWCRequests(),
            layer->getTransform(),
            layer->getDataspace(),
            isCompressData(&layer->getPrivateHandle()),
            layer->getPrivateHandle().pq_enable,
            layer->getPrivateHandle().pq_pos,
            layer->getPrivateHandle().pq_orientation,
            layer->getPrivateHandle().pq_table_idx,
            layer->isNeedPQ(),
            layer->isAIPQ(),
            layer->getPrivateHandle().usage,
            static_cast<unsigned int>(layer->getPrivateHandle().ext_info.status) & GRALLOC_EXTRA_MASK_YUV_COLORSPACE,
            layer->getLayerCaps());
    }
    dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");

    if (copy_invisible_layers.size())
    {
        dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");
        dump_str->appendFormat("| layer id |             handle / alloc_id |      fmt | blend |                                 comp | tr|         ds |c|        pq   |      usage | gralloc range  |   caps |\n");
        for (auto& layer : copy_invisible_layers)
        {
            dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");
            dump_str->appendFormat("|%9" PRId64 " | %18p /%9" PRId64 " | %8x | %5s | %3s(%8s,%3s,%3s,%5s:%5d) %3x| %2u| %10d |%d|%d,%3x,%d,%d,%d,%d|%12x|%16x|%8x|\n",
                layer->getId(),
                layer->getHandle(),
                layer->getPrivateHandle().alloc_id,
                layer->getPrivateHandle().format,
                getBlendString(layer->getBlend()),
                getCompString(layer->getCompositionType()),
                getHWLayerString(layer->getHwlayerType(), layer->getLayerCaps()),
                getCompString(layer->getSFCompositionType()),
                getCompString(layer->getLastCompTypeCallFromSF()),
                getCompFileString(layer->getHwlayerTypeFile()),
                layer->getHwlayerTypeLine(),
                layer->getHWCRequests(),
                layer->getTransform(),
                layer->getDataspace(),
                isCompressData(&layer->getPrivateHandle()),
                layer->getPrivateHandle().pq_enable,
                layer->getPrivateHandle().pq_pos,
                layer->getPrivateHandle().pq_orientation,
                layer->getPrivateHandle().pq_table_idx,
                layer->isNeedPQ(),
                layer->isAIPQ(),
                layer->getPrivateHandle().usage,
                static_cast<unsigned int>(layer->getPrivateHandle().ext_info.status) & GRALLOC_EXTRA_MASK_YUV_COLORSPACE,
                layer->getLayerCaps());
        }
        dump_str->appendFormat("+----------+-------------------------------+----------+-------+--------------------------------------+---+------------+-+-------------+------------+----------------+--------+\n");
    }

    for (auto& layer : copy_visible_layers)
    {
        dump_str->appendFormat("%9" PRId64 ": %s, usage: 0x%x flags: 0x%" PRIx64 "\n",
                               layer->getId(),
                               layer->getName().c_str(),
                               layer->getLayerUsage(),
                               layer->getMtkFlags());
    }
    if (copy_invisible_layers.size())
    {
        for (auto& layer : copy_invisible_layers)
        {
            dump_str->appendFormat("%9" PRId64 ": %s, usage: 0x%x flags: 0x%" PRIx64 "\n",
                                   layer->getId(),
                                   layer->getName().c_str(),
                                   layer->getLayerUsage(),
                                   layer->getMtkFlags());
        }
    }

    // This code flow is used for dump all display layers
    // If need this information, please enable this flow.

    /*
    dump_str->appendFormat("+----------+--------------+---------+-------+---------------------+---+------------+-+----|\n");
    dump_str->appendFormat("| layer id |       handle |     fmt | blend |           comp      | tr|     ds     |c| ct |\n");
    for (auto& kv : m_layers)
    {
        auto& layer = kv.second;
        dump_str->appendFormat("+----------+--------------+---------+-------+---------------------+---+------------+-+----|\n");
        dump_str->appendFormat("|%9" PRId64 " | %12p |%8x | %5s | %3s(%4s,%3s,%5d) | %2d| %10d |%d|%3d|\n",
                layer->getId(),
                layer->getHandle(),
                layer->getPrivateHandle().format,
                getBlendString(layer->getBlend()),
                getCompString(layer->getCompositionType()),
                getHWLayerString(layer->getHwlayerType()),
                getCompString(layer->getSFCompositionType()),
                layer->getHwlayerTypeLine(),
                layer->getTransform(),
                layer->getDataspace(),
                isCompressData(&layer->getPrivateHandle()),
                layer->isClientTarget()
                );
    }

    dump_str->appendFormat("+----------+--------------+---------+-------+---------------------+---+------------+-+\n");
    */
    if (m_histogram != nullptr)
    {
        m_histogram->dump(dump_str);
    }

    // dump supported color mode and render intent
    dump_str->appendFormat("Color Mode & Render Intent\n");
    dump_str->appendFormat("\tcurrent state: color_mode[%d] render_intent[%d] pq_mode_id[%d]\n",
            color_mode, render_intent, m_pq_mode_id.load());
    for (auto& item : m_color_mode_with_render_intent)
    {
        dump_str->appendFormat("\tColorMode %d: ", item.first);
        for (auto& intent : item.second)
        {
            dump_str->appendFormat("RenderIntent[%d]", intent.first);
            for (auto& info : intent.second)
            {
                dump_str->appendFormat("(id=%d range=%d),",
                        info.id, info.dynamic_range);
            }
            dump_str->appendFormat("");
        }
        dump_str->appendFormat("\n");
    }

    // dump CPU SET and scenario hint
    dump_str->appendFormat("\n");
    dump_str->appendFormat("CpuSet & Scenario:\n");
    dump_str->appendFormat("\tcpu_set: 0x%x\n", m_cpu_set);
    dump_str->appendFormat("\tscenario_hint: 0x%x\n", m_scenario_hint);
    dump_str->appendFormat("\n");

    m_color_transform->dump(dump_str);
    HWCMediator::getInstance().getOvlDevice(m_disp_id)->dump(m_disp_id, dump_str);
    mFpsCounter.dump(dump_str, "    ");
}

bool HWCDisplay::needDoAvGrouping(const unsigned int num_plugin_display)
{
    if (!Platform::getInstance().m_config.av_grouping ||
        (m_disp_id != HWC_DISPLAY_PRIMARY) ||
        m_sf_target_ts >= 0)
    {
        m_need_av_grouping = false;
        return m_need_av_grouping;
    }

    m_need_av_grouping = false;

    if ((getId() == HWC_DISPLAY_PRIMARY) && (num_plugin_display == 1))
    {
        auto&& layers = getVisibleLayersSortedByZ();
        int num_video_layer = 0;
        int num_secure_video_layer = 0;
        int num_camera_layer = 0;
        for (size_t i = 0; i < layers.size(); i++)
        {
            sp<HWCLayer> layer = layers[i];
            if (getGeTypeFromPrivateHandle(&layer->getPrivateHandle()) == GRALLOC_EXTRA_BIT_TYPE_VIDEO)
            {
                num_video_layer++;
                const unsigned int& usage = layer->getPrivateHandle().usage;
                if (usageHasSecure(usage))
                    num_secure_video_layer++;
            }

            if (getGeTypeFromPrivateHandle(&layer->getPrivateHandle()) == GRALLOC_EXTRA_BIT_TYPE_CAMERA)
            {
                num_camera_layer++;
            }
        }
        if (num_video_layer == 1) {
            int num_normal_video_layer = 0;
            num_normal_video_layer = num_video_layer - num_secure_video_layer;
            if ((Platform::getInstance().m_config.grouping_type & MTK_AV_GROUPING_SECURE_VP) &&
                    (num_secure_video_layer > 0))
            {
                m_need_av_grouping = true;
            }
            if ((Platform::getInstance().m_config.grouping_type & MTK_AV_GROUPING_NORMAL_VP) &&
                    (num_normal_video_layer > 0))
            {
                m_need_av_grouping = true;
            }
        }

        if ((Platform::getInstance().m_config.grouping_type & MTK_AV_GROUPING_CAMERA) && num_camera_layer == 1)
        {
            m_need_av_grouping = true;
        }
    }

    return m_need_av_grouping;
}

bool HWCDisplay::isForceGpuCompose()
{
    if (getColorTransformHint() != HAL_COLOR_TRANSFORM_IDENTITY &&
        !m_color_transform_ok)
    {
        return true;
    }

    if (getId() > HWC_DISPLAY_PRIMARY &&
        !Platform::getInstance().m_config.is_support_ext_path_for_virtual)
    {
        return true;
    }

    if (m_use_gpu_composition)
    {
        return true;
    }

#ifdef MTK_IN_DISPLAY_FINGERPRINT
    if (getIsHBM())
    {
        return true;
    }
#endif

    return false;
}

void HWCDisplay::setupPrivateHandleOfLayers()
{
    ATRACE_CALL();
    auto layers = getVisibleLayersSortedByZ();

    for (auto& layer : layers)
    {
#ifndef MTK_USER_BUILD
        char str[256];
        if (snprintf(str, sizeof(str), "setupPrivateHandle %d %d(%p)",
            layer->isStateChanged(), layer->isBufferChanged(), layer->getHandle()) > 0)
        {
            ATRACE_NAME(str);
        }
#endif
        layer->setPrevIsPQEnhance(layer->getPrivateHandle().pq_enable);
        if (layer->getHandle() != nullptr)
        {
            int err = setPrivateHandlePQInfo(layer->getHandle(), &(layer->getEditablePrivateHandle()));
            if (err != GRALLOC_EXTRA_OK)
            {
                HWC_LOGE("setPrivateHandlePQInfo err(%x), (handle=%p)", err, layer->getHandle());
            }
        }

        if (Platform::getInstance().m_config.always_setup_priv_hnd ||
            layer->isStateContentDirty() || layer->isBufferChanged())
        {
#ifndef MTK_USER_BUILD
            ATRACE_NAME("setupPrivateHandle");
#endif
            layer->setupPrivateHandle();
        }
    }

    if (layers.size() > 0)
    {
        // RGBA layer_0, alpha value don't care.
        auto& layer = layers[0];
        if (layer != nullptr &&
            layer->getHwcBuffer() != nullptr &&
            layer->getHwcBuffer()->getHandle() != nullptr &&
            layer->getPrivateHandle().format == HAL_PIXEL_FORMAT_RGBA_8888)
        {
            layer->getEditablePrivateHandle().format = HAL_PIXEL_FORMAT_RGBX_8888;

            layer->setMtkFlags(layer->getMtkFlags() | HWC_LAYER_FLAG_FORCE_RGBX);
        }
    }
}

void HWCDisplay::setOverrideMDPOutputFormatOfLayers()
{
    auto layers = getVisibleLayersSortedByZ();
    for (auto& layer : layers)
    {
        if (HwcFeatureList::getInstance().getFeature().is_support_pq &&
            HwcFeatureList::getInstance().getFeature().pq_video_whitelist_support &&
            HwcFeatureList::getInstance().getFeature().video_transition)
        {
            if ((layer->getHwlayerType() == HWC_LAYER_TYPE_MM) &&
                (getId() == HWC_DISPLAY_PRIMARY) &&
                (HWC_MIRROR_SOURCE_INVALID == getMirrorSrc()) &&
                layer->getPrivateHandle().pq_enable)
            {
                layer->setOverrideMDPOutputFormat(HAL_PIXEL_FORMAT_YUYV);
            }
            else
            {
                layer->setOverrideMDPOutputFormat(HWC_DISP_OVERRIDE_MDP_OUTPUT_FORMAT_DEFAULT);
            }
        }
    }
}

void HWCDisplay::setValiPresentState(HWC_VALI_PRESENT_STATE val, const int32_t& line)
{
    DispatcherJob* job = HWCDispatcher::getInstance().getExistJob(getId());
    HWC_ABORT_MSG("(%" PRIu64 ") (L%d) set s:%s jobID:%" PRIu64,
                  m_disp_id, line, getPresentValiStateString(val), (job) ? job->sequence : UINT64_MAX);
    m_vali_present_state = val;
}

void HWCDisplay::updateFps()
{
    if (mFpsCounter.update())
    {
        int32_t type = 0;
        getType(&type);
        HWC_LOGI("[Display_%" PRIu64 " (type:%d)] fps:%f,dur:%.2f,max:%.2f,min:%.2f",
                getId(), type, mFpsCounter.getFps(), mFpsCounter.getLastLogDuration() / 1e6,
                mFpsCounter.getMaxDuration() / 1e6, mFpsCounter.getMinDuration() / 1e6);
    }
}

void HWCDisplay::addUnpresentCount()
{
    m_prev_unpresent_count = m_unpresent_count;
    ++m_unpresent_count;
}

void HWCDisplay::decUnpresentCount()
{
    m_prev_unpresent_count = m_unpresent_count;
    --m_unpresent_count;
    if (m_unpresent_count < 0)
    {
        HWC_LOGE("%s error prepareFrame count(%" PRIu64 ":%d)", __func__, getId(), m_unpresent_count);
        m_unpresent_count = 0;
    }
}

void HWCDisplay::buildVisibleAndInvisibleLayer()
{
    removePendingRemovedLayers();
    buildVisibleAndInvisibleLayersSortedByZ();
    setupPrivateHandleOfLayers();
}

void HWCDisplay::setMSync2Enable(bool enable)
{
    if (m_msync_2_0_enable != enable)
    {
        m_msync_2_0_enable_changed = true;
        m_msync_2_0_enable = enable;
    }
}

void HWCDisplay::initColorHistogram(const sp<IOverlayDevice>& ovl)
{
    if (m_disp_id == HWC_DISPLAY_PRIMARY &&
            ovl->isHwcFeatureSupported(HWC_FEATURE_COLOR_HISTOGRAM))
    {
        m_histogram = std::make_shared<ColorHistogram>(m_disp_id);
    }
}

int32_t HWCDisplay::getContentSamplingAttribute(int32_t* format, int32_t* dataspace, uint8_t* mask)
{
    if (m_histogram == nullptr)
    {
        return HWC2_ERROR_UNSUPPORTED;
    }
    return m_histogram->getAttribute(format, dataspace, mask);
}

int32_t HWCDisplay::setContentSamplingEnabled(const int32_t enable, const uint8_t component_mask,
        const uint64_t max_frames)
{
    if (m_histogram == nullptr)
    {
        return HWC2_ERROR_UNSUPPORTED;
    }
    return m_histogram->setContentSamplingEnabled(enable, component_mask, max_frames);
}

int32_t HWCDisplay::getContentSample(const uint64_t max_frames, const uint64_t timestamp,
        uint64_t* frame_count, int32_t samples_size[4], uint64_t* samples[4])
{
    if (m_histogram == nullptr)
    {
        return HWC2_ERROR_UNSUPPORTED;
    }
    return m_histogram->getContentSample(max_frames, timestamp, frame_count, samples_size, samples);
}

int32_t HWCDisplay::addPresentInfo(unsigned int index, int fd, hwc2_config_t active_config)
{
    if (m_histogram == nullptr)
    {
        return NO_ERROR;
    }
    return m_histogram->addPresentInfo(index, fd, active_config);
}

void HWCDisplay::populateColorModeAndRenderIntent(const sp<IOverlayDevice>& ovl)
{
    // clear all data
    for (auto& item : m_color_mode_with_render_intent)
    {
        item.second.clear();
    }
    m_color_mode_with_render_intent.clear();

    // initialize color mode
    std::vector<int32_t> color_mode_list;
    if (m_disp_id == HWC_DISPLAY_PRIMARY)
    {
        int32_t color_mode = ovl->getSupportedColorMode();
        switch (color_mode)
        {
            case HAL_COLOR_MODE_DISPLAY_P3:
                color_mode_list.push_back(HAL_COLOR_MODE_NATIVE);
                color_mode_list.push_back(HAL_COLOR_MODE_SRGB);
                color_mode_list.push_back(HAL_COLOR_MODE_DISPLAY_P3);
                break;

            case HAL_COLOR_MODE_SRGB:
                color_mode_list.push_back(HAL_COLOR_MODE_NATIVE);
                color_mode_list.push_back(HAL_COLOR_MODE_SRGB);
                break;

            case HAL_COLOR_MODE_NATIVE:
                color_mode_list.push_back(HAL_COLOR_MODE_NATIVE);
                break;

            default:
                HWC_LOGW("%s: failed to initialize color mode, unknown color mode: %d",
                        __func__, color_mode);
                break;
        }
    }
    if (color_mode_list.empty())
    {
        color_mode_list.push_back(HAL_COLOR_MODE_NATIVE);
    }

    // initialize render intent
    size_t color_mode_count = color_mode_list.size();
    if (m_disp_id == HWC_DISPLAY_PRIMARY)
    {
        for (size_t i = 0; i < color_mode_count; i++)
        {
            const std::vector<PqModeInfo>& mode_info = getPqDevice()->getRenderIntent(color_mode_list[i]);
            for (size_t j = 0; j < mode_info.size(); j++)
            {
                const PqModeInfo& pq_info = mode_info[j];
                IntentInfo info = {
                    .id = pq_info.id,
                    .dynamic_range = pq_info.dynamic_range,
                    .intent = pq_info.intent,
                };
                m_color_mode_with_render_intent[pq_info.color_mode][pq_info.intent].push_back(info);
            }
        }
    }

    for (size_t i = 0; i < color_mode_count; i++)
    {
        int32_t color_mode = color_mode_list[i];
        if (m_color_mode_with_render_intent[color_mode].empty())
        {
            IntentInfo info_sdr = {
                .id = DEFAULT_PQ_MODE_ID,
                .dynamic_range = 0,
                .intent = HAL_RENDER_INTENT_COLORIMETRIC,
            };
            m_color_mode_with_render_intent[color_mode][HAL_RENDER_INTENT_COLORIMETRIC].push_back(info_sdr);
        }
    }
    updatePqModeId(true);
}

int32_t HWCDisplay::getColorModeList(uint32_t* out_num_modes, int32_t* out_modes)
{
    if (out_num_modes == nullptr)
    {
        HWC_LOGW("%s: out_num_modes is null", __func__);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    uint32_t num_mode = static_cast<uint32_t>(m_color_mode_with_render_intent.size());
    if (out_modes == nullptr)
    {
        *out_num_modes = num_mode;
        return HWC2_ERROR_NONE;
    }

    if (num_mode > *out_num_modes)
    {
        HWC_LOGW("%s: the size of out_num_modes is not enough[%u|%u]",
                __func__, *out_num_modes, num_mode);
    }
    else if (num_mode < *out_num_modes)
    {
        *out_num_modes = num_mode;
    }

    size_t i = 0;
    for (auto iter = m_color_mode_with_render_intent.begin();
            i < *out_num_modes; ++iter, i++)
    {
        out_modes[i] = iter->first;
    }
    return HWC2_ERROR_NONE;
}

int32_t HWCDisplay::setColorMode(const int32_t& color_mode)
{
    if (color_mode < 0)
    {
        return HWC2_ERROR_BAD_PARAMETER;
    }

    auto search = m_color_mode_with_render_intent.find(color_mode);
    if (search != m_color_mode_with_render_intent.end())
    {
        m_color_mode = color_mode;
        return HWC2_ERROR_NONE;
    }

    return HWC2_ERROR_UNSUPPORTED;
}

int32_t HWCDisplay::getRenderIntents(const int32_t mode, uint32_t* out_num_intents, int32_t* out_intents)
{
    if (out_num_intents == nullptr)
    {
        HWC_LOGW("%s: out_num_intents is null", __func__);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    if (mode < 0)
    {
        HWC_LOGW("%s: invalid color mode(%d)", __func__, mode);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    auto item = m_color_mode_with_render_intent.find(mode);
    if (item == m_color_mode_with_render_intent.end())
    {
        HWC_LOGW("%s: unsupported color mode(%d)", __func__, mode);
        return HWC2_ERROR_UNSUPPORTED;
    }

    uint32_t num_intent = static_cast<uint32_t>(m_color_mode_with_render_intent[mode].size());
    if (out_intents == nullptr)
    {
        *out_num_intents = num_intent;
        return HWC2_ERROR_NONE;
    }

    if (num_intent > *out_num_intents)
    {
        HWC_LOGW("%s: the size of out_num_intents is not enough[%u|%u]",
                __func__, *out_num_intents, num_intent);
    }
    else if (num_intent < *out_num_intents)
    {
        *out_num_intents = num_intent;
    }

    size_t i = 0;
    for (auto iter = m_color_mode_with_render_intent[mode].begin();
            i < *out_num_intents; ++iter, i++)
    {
        out_intents[i] = (*iter).first;
    }
    return HWC2_ERROR_NONE;
}

int32_t HWCDisplay::setColorModeWithRenderIntent(const int32_t mode, const int32_t intent)
{
    if (mode < 0)
    {
        HWC_LOGW("%s: invalid color mode(%d)", __func__, mode);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    if (intent < 0)
    {
        HWC_LOGW("%s: invalid render intent(%d)", __func__, intent);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    auto mode_table = m_color_mode_with_render_intent.find(mode);
    if (mode_table == m_color_mode_with_render_intent.end())
    {
        HWC_LOGW("%s: unsupported color mode(%d)", __func__, mode);
        return HWC2_ERROR_UNSUPPORTED;
    }

    auto intent_table = mode_table->second.find(intent);
    if (intent_table == mode_table->second.end() || intent_table->second.empty())
    {
        HWC_LOGW("%s: unsupported render intent(%d)", __func__, intent);
        return HWC2_ERROR_UNSUPPORTED;
    }

    {
        AutoMutex lock(m_dump_lock);
        m_color_mode = mode;
        m_render_intent = intent;
    }

    return HWC2_ERROR_NONE;
}

void HWCDisplay::updatePqModeId(bool force_update)
{
    if (!force_update && m_color_mode == m_prev_color_mode &&
            m_render_intent == m_prev_render_intent && m_hdr_type == m_prev_hdr_type)
    {
        return;
    }

    int32_t id = 0;
    auto color_mode = m_color_mode_with_render_intent.find(m_color_mode);
    if (color_mode != m_color_mode_with_render_intent.end())
    {
        auto intent = color_mode->second.find(m_render_intent);
        if (intent != color_mode->second.end())
        {
            for (auto& info : intent->second)
            {
                id = info.id;
                if ((m_hdr_type && info.dynamic_range == 1) ||
                        (!m_hdr_type && info.dynamic_range == 0))
                {
                    break;
                }
            }
        }
    }

    m_pq_mode_id = id;
    m_prev_color_mode = m_color_mode;
    m_prev_render_intent = m_render_intent;
    m_prev_hdr_type = m_hdr_type;
}

void HWCDisplay::setScenarioHint(ComposerExt::ScenarioHint flag, ComposerExt::ScenarioHint mask)
{
    AutoMutex lock(m_dump_lock);
    uint32_t new_val = static_cast<uint32_t>(flag) & static_cast<uint32_t>(mask);
    uint32_t old_val = static_cast<uint32_t>(m_scenario_hint) & static_cast<uint32_t>(mask);
    if (new_val == old_val)
    {
        return;
    }

    // the scenario has changed, so check cpu set
    if (static_cast<uint32_t>(mask) &
            static_cast<uint32_t>(ComposerExt::ScenarioHint::kVideoPlyback))
    {
        m_dirty_cpu_set = true;
    }

    // store the new hint of scenario
    uint32_t temp_val = static_cast<uint32_t>(m_scenario_hint);
    temp_val &= ~static_cast<uint32_t>(mask);
    temp_val |= static_cast<uint32_t>(new_val);
    m_scenario_hint = static_cast<ComposerExt::ScenarioHint>(temp_val);
}

unsigned int HWCDisplay::updateCpuSet()
{
    AutoMutex lock(m_dump_lock);
    // the state is not dirty, so ignore it
    if (!m_dirty_cpu_set)
    {
        return m_cpu_set;
    }

    m_dirty_cpu_set = false;
    m_cpu_set = HWC_CPUSET_NONE;
    // platform switch request that HWC always run on middle core
    if (Platform::getInstance().m_config.plat_switch & HWC_PLAT_SWITCH_ALWAYS_ON_MIDDLE_CORE)
    {
        m_cpu_set = HWC_CPUSET_MIDDLE;
        return m_cpu_set;
    }

    // Platform switch request that HWC run on middle at video playback
    if (Platform::getInstance().m_config.plat_switch & HWC_PLAT_SWITCH_VP_ON_MIDDLE_CORE)
    {
        if (static_cast<uint32_t>(m_scenario_hint) &
                static_cast<uint32_t>(ComposerExt::ScenarioHint::kVideoPlyback))
        {
            m_cpu_set = HWC_CPUSET_MIDDLE;
            return m_cpu_set;
        }
    }

    return m_cpu_set;
}

void HWCDisplay::setDirtyCpuSet()
{
    AutoMutex lock(m_dump_lock);
    m_dirty_cpu_set = true;
}

void HWCDisplay::extendSfTargetTime(DispatcherJob* job)
{
    // display can not process video and camera buffer directly, so we always need MDP and MML
    // in these scenario. therefore we also extend it when this layer is composed by GPU. then
    // we can avoid adjust sf target time constantly when their composition type change.
    nsecs_t period = 0;
    if ((m_num_video_layer != 0 && Platform::getInstance().m_config.plat_switch &
            HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_VIDEO)
            || (m_num_camera_layer != 0 && Platform::getInstance().m_config.plat_switch &
            HWC_PLAT_SWITCH_EXTEND_SF_TARGET_TS_FOR_CAMERA))
    {
        period = getVsyncPeriod(job->active_config);
        job->sf_target_ts += period;
        job->present_after_ts = job->sf_target_ts - period;
        if (ATRACE_ENABLED())
        {
            char atrace_tag[128];
            const nsecs_t cur_time = systemTime();
            if (snprintf(atrace_tag, sizeof(atrace_tag),
                         "(%" PRIu64 ") extend ts, sf ts: %" PRId64 " cur %" PRId64
                         " diff %" PRId64 ".%" PRId64 "ms",
                         m_disp_id,
                         job->sf_target_ts, cur_time,
                         (job->sf_target_ts - cur_time) / 1000000,
                         (job->sf_target_ts - cur_time) % 1000000) > 0)
            {
                HWC_ATRACE_NAME(atrace_tag);
            }
            HWC_ATRACE_INT64(m_perf_extend_sf_target_time_str.c_str(), period);
        }
    }

    if (period != m_prev_extend_sf_target_time)
    {
        m_prev_extend_sf_target_time = period;
        HWC_LOGI("(%" PRIu64 ") extend sf target time: %" PRId64, m_disp_id,
                m_prev_extend_sf_target_time);
    }
    HWC_ATRACE_INT64(m_perf_extend_sf_target_time_str.c_str(), period);
}
