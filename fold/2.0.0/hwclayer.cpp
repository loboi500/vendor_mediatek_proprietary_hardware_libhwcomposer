#define DEBUG_LOG_TAG "HWCLayer"
#include "hwclayer.h"

#include <hwc_feature_list.h>

#include "overlay.h"
#include "dispatcher.h"
#include "glai_controller.h"
#include "hwcdisplay.h"
#include "hwcbuffer.h"
#include "platform_wrap.h"
#include "queue.h"
#include "utils/transform.h"
#include "grallocdev.h"

#include <algorithm>

std::atomic<uint64_t> HWCLayer::id_count(0);

HWCLayer::HWCLayer(const wp<HWCDisplay>& disp, const uint64_t& disp_id, const bool& is_ct)
    : m_mtk_flags(0)
    , m_id(++id_count)
    , m_is_ct(is_ct)
    , m_disp(disp)
    , m_hwlayer_type(HWC_LAYER_TYPE_NONE)
    , m_hwlayer_type_line(-1)
    , m_hwlayer_type_file(HWC_COMP_FILE_UNK)
    , m_sf_comp_type(HWC2_COMPOSITION_INVALID)
    , m_returned_comp_type(HWC2_COMPOSITION_INVALID)
    , m_is_composited_by_hwc(false)
    , m_dataspace(0)
    , m_blend(HWC2_BLEND_MODE_NONE)
    , m_plane_alpha(0.0f)
    , m_z_order(0)
    , m_transform(0)
    , m_state_changed(0)
    , m_disp_id(disp_id)
    , m_hwc_buf(new HWCBuffer(m_disp_id, static_cast<int64_t>(m_id), is_ct))
    , m_is_visible(false)
    , m_sf_comp_type_call_from_sf(0)
    , m_last_comp_type_call_from_sf(0)
    , m_layer_caps(0)
    , m_layer_color(0)
    , m_hdr_type(MTK_METADATA_TYPE_NONE)
    , m_per_frame_metadata_changed(false)
    , m_prev_pq_enable(false)
    , m_need_pq(false)
    , m_last_app_game_pq(false)
    , m_last_ai_pq(false)
    , m_last_camera_preview_hdr(false)
    , m_queue(nullptr)
    , m_has_set_perf(false)
    , m_hwc_requests(0)
    , m_layer_usage(HWC_LAYER_USAGE_NONE)
    , m_glai_agent_id(-1)
    , m_glai_last_inference(false)
    , m_debug_type(HWC_DEBUG_LAYER_TYPE_NONE)
    , m_unchanged_cnt(0)
    , m_inactive_cnt(0)
    , m_inactive_set_hint(false)
    , m_is_inactive_client_layer(false)
    , m_fbt_unchanged_hint(false)
    , m_unchanged_ratio_valid(false)
    , m_prev_hwlayer_type(HWC_LAYER_TYPE_NONE)
    , m_prev_hwlayer_type_line(-1)
    , m_prev_hwlayer_type_file(HWC_COMP_FILE_UNK)
    , m_brightness(0.0f)
    , m_color_transform(new ColorTransform(HAL_COLOR_TRANSFORM_IDENTITY, false, false))
{
    memset(&m_damage, 0, sizeof(m_damage));
    memset(&m_display_frame, 0, sizeof(m_display_frame));
    memset(&m_source_crop, 0, sizeof(m_source_crop));
    memset(&m_visible_region, 0, sizeof(m_visible_region));
    memset(&m_mdp_dst_roi, 0, sizeof(m_mdp_dst_roi));
    memset(&m_glai_dst_roi, 0, sizeof(m_glai_dst_roi));
    memset(&m_blocking_region, 0, sizeof(m_blocking_region));
    memset(&m_mml_cfg, 0, sizeof(mml_frame_info));

    if (m_hwc_buf == nullptr)
        HWC_LOGE("%s allocate HWCBuffer for m_hwc_buf fail", __func__);
}

HWCLayer::~HWCLayer()
{
    if (m_damage.rects != nullptr)
        free((void*)m_damage.rects);

    if (m_visible_region.rects != nullptr)
        free((void*)m_visible_region.rects);

    if (m_glai_agent_id > 0)
    {
        GlaiController::getInstance().cleanModel(m_glai_agent_id);
    }
}

#define SET_LINE_NUM(RTLINE, TYPE) ({ \
                            *RTLINE = __LINE__; \
                            TYPE; \
                        })

String8 HWCLayer::toString8()
{
    auto& display_frame = getDisplayFrame();
    auto& src_crop = getSourceCrop();

    String8 ret;
    ret.appendFormat("id:%" PRIu64 " v:%d acq:%d usage:%x,%d,%" PRIu64 " w:%d,%d h:%d,%d f:%x sz:%d z:%u ds:%x c:%x %s(%s,%s,%d) s[%.1f,%.1f,%.1f,%.1f]->d[%d,%d,%d,%d] t:%u d(s%d,sr0x%x,b%d) fbdc:%d(isG2G:%d) pq:%d g_hdr:%d cp_hdr:%d sec:%u aipq:%d",
        getId(),
        isVisible(),
        getAcquireFenceFd(),
        getPrivateHandle().usage,
        getPrivateHandle().ion_fd,
        getPrivateHandle().alloc_id,
        getPrivateHandle().width,
        getPrivateHandle().y_stride,
        getPrivateHandle().height,
        getPrivateHandle().vstride,
        getPrivateHandle().format,
        getPrivateHandle().size,
        getZOrder(),
        getDataspace(),
        getLayerColor(),
        getCompString(getReturnedCompositionType()),
        getHWLayerString(getHwlayerType()),
        getCompString(getSFCompositionType()),
        getHwlayerTypeLine(),
        src_crop.left,
        src_crop.top,
        src_crop.right,
        src_crop.bottom,
        display_frame.left,
        display_frame.top,
        display_frame.right,
        display_frame.bottom,
        getTransform(),
        isStateChanged(),
        getStateChangedReason(),
        isBufferChanged(),
        isCompressData(&getPrivateHandle()),
        isG2GCompressData(&getPrivateHandle()),
        isNeedPQ(),
        isGameHDR(),
        isCameraPreviewHDR(),
        isSecure(&getPrivateHandle()),
        isAIPQ());
    return ret;
}

// return final transform rectify with prexform
uint32_t HWCLayer::getXform() const
{
    uint32_t xform = getTransform();
    const PrivateHandle& priv_hnd = getPrivateHandle();
    rectifyXformWithPrexform(&xform, priv_hnd.prexform);
    return xform;
}

bool HWCLayer::decideMdpOutputCompressedBuffers(const HWCDisplay& disp) const
{
    if (!Platform::getInstance().m_config.mdp_support_compress)
    {
        return false;
    }

    uint32_t format = decideMdpOutputFormat(disp);
    if (format != HAL_PIXEL_FORMAT_RGBA_8888 &&
        format != HAL_PIXEL_FORMAT_RGBX_8888 &&
        format != HAL_PIXEL_FORMAT_RGBA_1010102)
    {
        return false;
    }

    if (!needRotate() && (isNeedPQ() || isGameHDR()))
    {
        return true;
    }

    return false;
}

uint32_t HWCLayer::decideMdpOutputFormat(const HWCDisplay& disp) const
{
    uint32_t output_format = getPrivateHandle().format;
    if (Platform::getInstance().m_config.force_mdp_output_format != 0)
    {
        output_format = Platform::getInstance().m_config.force_mdp_output_format;
    }
    else if (isAIPQ())
    {
        output_format = HAL_PIXEL_FORMAT_YUYV;
    }
    else if (HwcFeatureList::getInstance().getFeature().is_support_pq &&
             HwcFeatureList::getInstance().getFeature().pq_video_whitelist_support &&
             disp.isInternal() && getPrivateHandle().pq_enable)
    {
        output_format = HAL_PIXEL_FORMAT_YUYV;
    }
    else if (isCameraPreviewHDR())
    {
        output_format = HAL_PIXEL_FORMAT_RGBA_1010102;
    }

    return convertFormat4Bliter(output_format);
}

bool HWCLayer::needRotate() const
{
    return getXform() != Transform::ROT_0;
}

bool HWCLayer::needScaling() const
{
    if (getXform() & HAL_TRANSFORM_ROT_90)
    {
        return (WIDTH(getSourceCrop()) != HEIGHT(getDisplayFrame())) ||
                (HEIGHT(getSourceCrop()) != WIDTH(getDisplayFrame()));
    }

    return (WIDTH(getSourceCrop()) != WIDTH(getDisplayFrame())) ||
            (HEIGHT(getSourceCrop()) != HEIGHT(getDisplayFrame()));
}

bool HWCLayer::hasAlpha() const {
    return (m_plane_alpha < 1.0f) || ((getBlend() != HWC2_BLEND_MODE_NONE) && isTransparentFormat(getPrivateHandle().format));
}

bool HWCLayer::isPixelAlphaUsed() const {
    HWC_LOGV("%s(), id:%" PRIu64 ", Blend mode = %s format = %d", __FUNCTION__, getId(), getBlendString(m_blend), getPrivateHandle().format);
    return ((getBlend() != HWC2_BLEND_MODE_NONE) && isTransparentFormat(getPrivateHandle().format));
}

bool HWCLayer::checkSkipValidate(const HWCDisplay& disp, const bool& is_validate_call)
{
    // if the inactiveLayer of the m_current_set only State Changed by HWC_LAYER_STATE_CHANGE_HWC_CLIENT_TYPE,
    // measns HRT marked GPU Type, may skip hrt
    if (disp.isSupportSmartComposition() && is_validate_call &&
        disp.hasValidCurrentSet() &&
        isValidInactiveLayer() &&
        isInactiveClientLayer() &&
        isStateChanged() &&
        getStateChangedReason() == HWC_LAYER_STATE_CHANGE_HWC_CLIENT_TYPE)
    {
        // for validate check skip hrt
        HWC_LOGV("skip HRT(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "preHWLayerType: %d, SF type:%d, SFcall type:%d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 getPrevHwlayerType(), getReturnedCompositionType(), getSFCompositionType());
    }
    else if (isStateChanged())
    {
        // for present check skip validate
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "isStateChanged: %d, getStateChangedReason: 0x%x",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 isStateChanged(), getStateChangedReason());
        return false;
    }

    if (!is_validate_call &&
        getHwlayerType() == HWC_LAYER_TYPE_INVALID)
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "getHwlayerType: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 getHwlayerType());
        return false;
    }

    if (getPrevIsPQEnhance() != getPrivateHandle().pq_enable)
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "getPrevIsPQEnhance: %d, pq_enable: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 getPrevIsPQEnhance(), getPrivateHandle().pq_enable);
        return false;
    }

    if (isNeedPQ(1) ||
        isNeedPQ(1) != getLastAppGamePQ())
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "isNeedPQ(1): %d, getLastAppGamePQ: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 isNeedPQ(1), getLastAppGamePQ());
        return false;
    }

    if (isAIPQ() != getLastAIPQ())
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "isAIPQ: %d, getLastAIPQ: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 isAIPQ(), getLastAIPQ());
        return false;
    }

    if (isCameraPreviewHDR() != getLastCameraPreviewHDR())
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "isCameraPreviewHDR: %d, getLastCameraPreviewHDR: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 isCameraPreviewHDR(), getLastCameraPreviewHDR());
        return false;
    }

    if (getPrivateHandle().glai_inference != getGlaiLastInference())
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "glai_inference: %d, getGlaiLastInference: %d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 getPrivateHandle().glai_inference, getGlaiLastInference());
        return false;
    }

    if (disp.isSupportBWMonitor() && isValidUnchangedLayer()
        && !Platform::getInstance().m_config.bwm_skip_hrt_calc
        && (getPrevHwlayerType() == HWC_LAYER_TYPE_UI) &&
        (!isUnchangedLayer() || !isUnchangedRatioValid()))// unchanged layer update
    {
        HWC_LOGD("no skip vali(%" PRIu64 "-%" PRIu64 ":L%d) validate(%d), "
                 "isUnchanged:%d, isRatioValid:%d, ucnt:%d",
                 m_disp_id, m_id, __LINE__, is_validate_call,
                 isUnchangedLayer(), isUnchangedRatioValid(), getUnchangedCnt());
        return false;
    }

    return true;
}

void HWCLayer::validate(const HWCDisplay& disp, int32_t pq_mode_id)
{
    const int& compose_level = Platform::getInstance().m_config.compose_level;
    int32_t line = -1;
    // isUILayerValid and isMMLayerValid should set the file value, then
    // we can know the reason. However it need to modify the interface and
    // implementation of whole platform. We will do it later.
    HWC_COMP_FILE file = HWC_COMP_FILE_NSET;
    int32_t sf_comp_types_before_valid = getReturnedCompositionType();

    if (getSFCompTypeCallFromSF() == true)
    {
        sf_comp_types_before_valid = getSFCompositionType();
    }

    if (sf_comp_types_before_valid == HWC2_COMPOSITION_CLIENT ||
        isHint(HWC_LAYER_TYPE_INVALID))
    {
        setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    if (sf_comp_types_before_valid == HWC2_COMPOSITION_SOLID_COLOR ||
        isHint(HWC_LAYER_TYPE_DIM))
    {
        if (WIDTH(m_display_frame) <= 0 || HEIGHT(m_display_frame) <= 0)
        {
            setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
            return;
        }
        setHwlayerType(HWC_LAYER_TYPE_DIM, __LINE__, HWC_COMP_FILE_HWCL);

        return;
    }

    // checking handle cannot be placed before checking dim layer
    // because handle of dim layer is nullptr.
    if (getHwcBuffer() == nullptr || getHwcBuffer()->getHandle() == nullptr)
    {
        setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    // for drm video
    const unsigned int buffer_type = getGeTypeFromPrivateHandle(&getPrivateHandle());

    if (usageHasProtected(getPrivateHandle().usage) && !disp.getSecure())
    {
        setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    if (usageHasSecure(getPrivateHandle().usage))
    {
        if (buffer_type == GRALLOC_EXTRA_BIT_TYPE_VIDEO)
        {
            // Video source is protected by DRM and sink directly, so we can
            // ignore related check
        }
        else if (!disp.getSecure() && !disp.isInternal())
        {
            setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
            return;
        }
    }

    // try use MM via hint
    if (isHint(HWC_LAYER_TYPE_MM))
    {
        if (Platform::getInstance().isMMLayerValid(this, pq_mode_id, &line))
        {
            setHwlayerType(HWC_LAYER_TYPE_MM, __LINE__, HWC_COMP_FILE_HWCL);
            return;
        }
    }

    // try use UI via hint
    if (isHint(HWC_LAYER_TYPE_UI))
    {
        if (Platform::getInstance().isUILayerValid(this, &line))
        {
            setHwlayerType(HWC_LAYER_TYPE_UI, __LINE__, HWC_COMP_FILE_HWCL);
            return;
        }
    }

    // for glai layer
    if ((compose_level & COMPOSE_DISABLE_GLAI) == 0 &&
        Platform::getInstance().isGlaiLayerValid(this, &line))
    {
        setHwlayerType(HWC_LAYER_TYPE_GLAI, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    // for ui layer
    if ((compose_level & COMPOSE_DISABLE_UI) == 0 &&
        Platform::getInstance().isUILayerValid(this, &line))
    {
        if (getSFCompositionType() == HWC2_COMPOSITION_CURSOR)
        {
            setHwlayerType(HWC_LAYER_TYPE_CURSOR, __LINE__, HWC_COMP_FILE_HWCL);
            return;
        }

        setHwlayerType(HWC_LAYER_TYPE_UI, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    // for mdp layer
    if (compose_level & COMPOSE_DISABLE_MM)
    {
        setHwlayerType(HWC_LAYER_TYPE_INVALID, __LINE__, HWC_COMP_FILE_HWCL);
        return;
     }

    if (Platform::getInstance().isMMLayerValid(this, pq_mode_id, &line))
    {
        setHwlayerType(HWC_LAYER_TYPE_MM, __LINE__, HWC_COMP_FILE_HWCL);
        return;
    }

    setHwlayerType(HWC_LAYER_TYPE_INVALID, line == -1 ? line : line + 10000, file);
}

void HWCLayer::calculateMdpDstRoi(const HWCDisplay& disp)
{
    if (getHwlayerType() != HWC_LAYER_TYPE_MM)
        return;

    if (!disp.isRpoSupported())
    {
        m_mdp_dst_roi = getDisplayFrame();
        return;
    }

    const bool need_mdp_rot = (getLayerCaps() & HWC_MDP_ROT_LAYER);
    const bool need_mdp_rsz = (getLayerCaps() & HWC_MDP_RSZ_LAYER);

    m_mdp_dst_roi.left = 0;
    m_mdp_dst_roi.top = 0;

    // process rotation
    if (need_mdp_rot)
    {
        switch (getTransform())
        {
            case HAL_TRANSFORM_ROT_90:
            case HAL_TRANSFORM_ROT_270:
            case HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_H:
            case HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_V:
                m_mdp_dst_roi.right  = static_cast<int>(HEIGHT(getSourceCrop()));
                m_mdp_dst_roi.bottom = static_cast<int>(WIDTH(getSourceCrop()));
                break;

            default:
                m_mdp_dst_roi.right  = static_cast<int>(WIDTH(getSourceCrop()));
                m_mdp_dst_roi.bottom = static_cast<int>(HEIGHT(getSourceCrop()));
                break;
        }
    }
    else
    {
        m_mdp_dst_roi.right  = static_cast<int>(WIDTH(getSourceCrop()));
        m_mdp_dst_roi.bottom = static_cast<int>(HEIGHT(getSourceCrop()));
    }

    //process scaling
    if (need_mdp_rsz)
    {
        const int32_t src_w = WIDTH(m_mdp_dst_roi);
        const int32_t src_h = HEIGHT(m_mdp_dst_roi);
        const int32_t dst_w = WIDTH(getDisplayFrame());
        const int32_t dst_h = HEIGHT(getDisplayFrame());

        const int32_t max_src_w_of_disp_rsz =
            static_cast<int32_t>(HWCMediator::getInstance().getOvlDevice(m_disp_id)->getRszMaxWidthInput());

        const bool is_any_shrank = (src_w > dst_w || src_h > dst_h);

        if (!is_any_shrank)
        {
            double max_width_scale_percentage_of_mdp = 0.0f;
            if (dst_w > src_w && max_src_w_of_disp_rsz > src_w) {
                max_width_scale_percentage_of_mdp = static_cast<double>(max_src_w_of_disp_rsz - src_w) / (dst_w - src_w);
            }
            else
            {
                max_width_scale_percentage_of_mdp = 1.0f;
            }

            const double final_mdp_scale_percentage = std::min(max_width_scale_percentage_of_mdp, Platform::getInstance().m_config.mdp_scale_percentage);

            const int& dst_l = 0;
            const int& dst_t = 0;
            m_mdp_dst_roi.right = dst_l +
                                  static_cast<int>(src_w * (1 - final_mdp_scale_percentage)) +
                                  static_cast<int>(dst_w * final_mdp_scale_percentage);
            m_mdp_dst_roi.bottom = dst_t +
                                   static_cast<int>(src_h * (1 - Platform::getInstance().m_config.mdp_scale_percentage)) +
                                   static_cast<int>(dst_h * Platform::getInstance().m_config.mdp_scale_percentage);
        }
        else
        {
            m_mdp_dst_roi.right = dst_w;
            m_mdp_dst_roi.bottom = dst_h;
        }
    }
    else
    {
        m_mdp_dst_roi.right = WIDTH(m_mdp_dst_roi);
        m_mdp_dst_roi.bottom = HEIGHT(m_mdp_dst_roi);
    }

    const DisplayData* display_data = DisplayManager::getInstance().getDisplayData(m_disp_id);
    if (display_data != nullptr && m_mdp_dst_roi.right > display_data->width)
    {
        m_mdp_dst_roi.right = display_data->width;
    }
    if (display_data != nullptr && m_mdp_dst_roi.bottom > display_data->height)
    {
        m_mdp_dst_roi.bottom = display_data->height;
    }

    HWC_LOGD("(%" PRIu64 ":%" PRIu64 ") %s MDP Dst ROI:%d,%d,%d,%d tr:%d s[%.1f,%.1f,%.1f,%.1f]->d[%d,%d,%d,%d]",
             m_disp_id, m_id, __func__,
             m_mdp_dst_roi.left, m_mdp_dst_roi.top,
             m_mdp_dst_roi.right, m_mdp_dst_roi.bottom,
             getTransform(),
             getSourceCrop().left, getSourceCrop().top, getSourceCrop().right, getSourceCrop().bottom,
             getDisplayFrame().left, getDisplayFrame().top, getDisplayFrame().right, getDisplayFrame().bottom);
}

bool HWCLayer::isGameHDR() const
{
    if (CC_UNLIKELY(!HwcFeatureList::getInstance().getFeature().game_hdr))
    {
        return false;
    }

    const uint32_t&& producer = static_cast<uint32_t>(getPrivateHandle().ext_info.status) & GRALLOC_EXTRA_MASK_TYPE;
    return (producer == GRALLOC_EXTRA_BIT_TYPE_GPU &&
        getPrivateHandle().format == HAL_PIXEL_FORMAT_RGBA_1010102 &&
        getDataspace() == HAL_DATASPACE_BT2020_PQ);
}

bool HWCLayer::isCameraPreviewHDR() const
{
    unsigned int gralloc_producer =
        static_cast<unsigned int>(getPrivateHandle().ext_info.status) & GRALLOC_EXTRA_MASK_TYPE;
    unsigned int gralloc_color_range =
        static_cast<unsigned int>(getPrivateHandle().ext_info.status) & GRALLOC_EXTRA_MASK_YUV_COLORSPACE;
    return gralloc_producer == GRALLOC_EXTRA_BIT_TYPE_CAMERA &&
           (getPrivateHandle().format == HAL_PIXEL_FORMAT_YCBCR_P010 ||
            getPrivateHandle().format == HAL_PIXEL_FORMAT_RGBA_1010102)&&
           (gralloc_color_range == GRALLOC_EXTRA_BIT_YUV_BT2020_NARROW ||
            m_dataspace == HAL_DATASPACE_BT2020_ITU_PQ);
}

bool HWCLayer::isHint(int32_t hint_hwlayer_type) const
{
    return getSFCompositionType() != HWC2_COMPOSITION_CLIENT &&
           (Platform::getInstance().m_config.hint_id > 0 ||
            !Platform::getInstance().m_config.hint_name.empty()) &&
           (Platform::getInstance().m_config.hint_id == m_id ||
            (Platform::getInstance().m_config.hint_name_shift < m_name.size() &&
             m_name.compare(
                 Platform::getInstance().m_config.hint_name_shift,
                 Platform::getInstance().m_config.hint_name.size(),
                 Platform::getInstance().m_config.hint_name) == 0))&&
           Platform::getInstance().m_config.hint_hwlayer_type == hint_hwlayer_type &&
           ((Platform::getInstance().m_config.compose_level & COMPOSE_DISABLE_MM) == 0 ||
            hint_hwlayer_type != HWC_LAYER_TYPE_MM) &&
           ((Platform::getInstance().m_config.compose_level & COMPOSE_DISABLE_UI) == 0 ||
            hint_hwlayer_type != HWC_LAYER_TYPE_UI);
}

int32_t HWCLayer::decideMdpOutDataspace() const {
    if (getHwlayerType() == HWC_LAYER_TYPE_MM && (isGameHDR() || isCameraPreviewHDR()))
    {
        return HAL_DATASPACE_BT709;
    }
    else
    {
        // in BT2020 case, dst will be set as BT709
        int32_t dataspace = getDataspace();
        return ((dataspace & HAL_DATASPACE_STANDARD_MASK) != HAL_DATASPACE_STANDARD_BT2020) ?
            dataspace : HAL_DATASPACE_BT709;
    }
}

int32_t HWCLayer::afterPresent(const bool& should_clear_state)
{
    // Careful!!! HWCBuffer::afterPresent() should be behind of the layer with zero
    // width or height checking
    if (getHwcBuffer() != nullptr)
        getHwcBuffer()->afterPresent(should_clear_state, isClientTarget());

    if (should_clear_state)
        clearStateChanged();

    setPerFrameMetadataChanged(false);
    setVisible(false);
    setLastAppGamePQ(isNeedPQ(1));
    setLastAIPQ(isAIPQ());
    setLastCameraPreviewHDR(isCameraPreviewHDR());
    setGlaiLastInference(getPrivateHandle().glai_inference);

    if (Platform::getInstance().m_config.is_smart_composition_support && HWC_DISPLAY_PRIMARY == m_disp_id)
    {
        setInactiveHint(false, false);// clear the hint, life cycle is per-job
    }
    initPrevHwlayerType(getHwlayerType(), getHwlayerTypeLine(), getHwlayerTypeFile());
    return 0;
}

void HWCLayer::setHwlayerType(const int32_t& hwlayer_type, const int32_t& line, const HWC_COMP_FILE& file)
{
    if ((m_hwlayer_type == HWC_LAYER_TYPE_MM && hwlayer_type != HWC_LAYER_TYPE_MM) ||
        (m_hwlayer_type == HWC_LAYER_TYPE_GLAI && hwlayer_type != HWC_LAYER_TYPE_GLAI))
    {
        if (m_queue)
        {
            HWC_LOGI("%s(), id:%" PRIu64 ", release queue sp, type %d -> %d", __FUNCTION__, getId(),
                     m_hwlayer_type, hwlayer_type);
            m_queue = nullptr;
        }
    }

    if (hwlayer_type == HWC_LAYER_TYPE_INVALID)
    {
        m_is_composited_by_hwc = false;
    }
    else
    {
        m_is_composited_by_hwc = true;
    }

    m_hwlayer_type = hwlayer_type;
    m_hwlayer_type_line = line;
    m_hwlayer_type_file = file;

    updateReturnedCompositionType();
}

void HWCLayer::toBeDim()
{
    m_priv_hnd.format = HAL_PIXEL_FORMAT_DIM;
}

void HWCLayer::updateReturnedCompositionType()
{
    int32_t sf_type = getSFCompositionType();
    switch (m_hwlayer_type) {
        case HWC_LAYER_TYPE_NONE:
            m_returned_comp_type = HWC2_COMPOSITION_INVALID;
            break;

        case HWC_LAYER_TYPE_INVALID:
            m_returned_comp_type = HWC2_COMPOSITION_CLIENT;
            break;

        case HWC_LAYER_TYPE_FBT:
        case HWC_LAYER_TYPE_UI:
        case HWC_LAYER_TYPE_MM:
        case HWC_LAYER_TYPE_GLAI:
        case HWC_LAYER_TYPE_IGNORE:
        case HWC_LAYER_TYPE_WORMHOLE:
            if (sf_type == HWC2_COMPOSITION_SOLID_COLOR)
            {
                m_returned_comp_type = HWC2_COMPOSITION_SOLID_COLOR;
            }
            else if (sf_type == HWC2_COMPOSITION_CURSOR)
            {
                //TODO we do not support setCursorPosition, so we should not return type with CURSOR
                m_returned_comp_type = HWC2_COMPOSITION_DEVICE;
            }
            else
            {
                m_returned_comp_type = HWC2_COMPOSITION_DEVICE;
            }
            break;

        case HWC_LAYER_TYPE_DIM:
            m_returned_comp_type = HWC2_COMPOSITION_SOLID_COLOR;
            break;

        case HWC_LAYER_TYPE_CURSOR:
            //TODO we do not support setCursorPosition, so we should not return type with CURSOR
            m_returned_comp_type = HWC2_COMPOSITION_DEVICE;
            break;

        default:
            m_returned_comp_type = HWC2_COMPOSITION_CLIENT;
            HWC_LOGW("%s: unknown HW layer type(%d)", __func__, m_hwlayer_type);
            break;
    }
}

void HWCLayer::setHandle(const buffer_handle_t& hnd)
{
    m_hwc_buf->setHandle(hnd);
}

void HWCLayer::setReleaseFenceFd(const int32_t& fence_fd, const bool& is_disp_connected)
{
    m_hwc_buf->setReleaseFenceFd(fence_fd, is_disp_connected);
}

void HWCLayer::setPrevReleaseFenceFd(const int32_t& fence_fd, const bool& is_disp_connected)
{
    m_hwc_buf->setPrevReleaseFenceFd(fence_fd, is_disp_connected);
}

void HWCLayer::setAcquireFenceFd(const int32_t& acquire_fence_fd)
{
    m_hwc_buf->setAcquireFenceFd(acquire_fence_fd);
}

void HWCLayer::setDataspace(const int32_t& dataspace)
{
    if (m_dataspace != dataspace)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_DATASPACE);
        m_dataspace = dataspace;
    }
}

int32_t HWCLayer::getDataspace() const
{
    // When CameraPreviewHDR is AOSP S, it can not set correct dataspace on layer.
    // TODO: Remove this after AOSP T
    if (isCameraPreviewHDR())
    {
        return HAL_DATASPACE_BT2020_ITU_PQ;
    }
    return m_dataspace;
}

void HWCLayer::setDamage(const hwc_region_t& damage)
{
    if (!isHwcRegionEqual(m_damage, damage))
    {
        copyHwcRegion(&m_damage, damage);
    }
}

void HWCLayer::setBlend(const int32_t& blend)
{
    if (m_blend != blend)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_BLEND);
        m_blend = blend;
    }
}

void HWCLayer::setDisplayFrame(const hwc_rect_t& display_frame)
{
    if (memcmp(&m_display_frame, &display_frame, sizeof(hwc_rect_t)) != 0)
    {
        if (m_display_frame.left != display_frame.left ||
            m_display_frame.top != display_frame.top)
        {
            setStateChanged(HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_OFFSET);
        }

        if (WIDTH(m_display_frame) != WIDTH(display_frame) ||
            HEIGHT(m_display_frame) != HEIGHT(display_frame))
        {
            setStateChanged(HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_SIZE);
        }
        m_display_frame = display_frame;
    }
}

void HWCLayer::setSourceCrop(const hwc_frect_t& source_crop)
{
    if (memcmp(&m_source_crop, &source_crop, sizeof(hwc_frect_t)) != 0)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_SOURCE_CROP);
        m_source_crop = source_crop;
    }
}

void HWCLayer::setPlaneAlpha(const float& plane_alpha)
{
    if (m_plane_alpha != plane_alpha)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_PLANE_ALPHA);
        m_plane_alpha = plane_alpha;
    }
}

void HWCLayer::setZOrder(const uint32_t& z_order)
{
    if (m_z_order != z_order)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_ZORDER);
        m_z_order = z_order;
    }
}

void HWCLayer::setTransform(const unsigned int& transform)
{
    if (m_transform != transform)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_TRANSFORM);
        m_transform = static_cast<unsigned int>(transform);
    }
}

void HWCLayer::setVisibleRegion(const hwc_region_t& visible_region)
{
    if (!isHwcRegionEqual(m_visible_region, visible_region))
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_VISIBLE_REGION);
        copyHwcRegion(&m_visible_region, visible_region);
    }
}

void HWCLayer::setLayerColor(const hwc_color_t& color)
{
    uint32_t new_color = static_cast<uint32_t>(color.a << 24 | color.r << 16 | color.g << 8 | color.b);
    if (m_layer_color != new_color)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_COLOR);
        m_layer_color = new_color;
    }
}

void HWCLayer::initValidate()
{
    m_layer_caps = 0;
    m_hwc_requests = 0;
}

bool HWCLayer::isLayerPq() const
{
    return ((m_layer_caps & HWC_MDP_HDR_LAYER) != 0 || // for HwDec VP HDR, Game HDR, Camera Preview HDR
            getPrivateHandle().pq_enable || // for OSIE
            isAIPQ()); // for AIPQ, AISDR2HDR
}

void HWCLayer::completeLayerCaps(bool is_support_mml)
{
    bool is_game_hdr = isGameHDR();

    if (HWC_LAYER_TYPE_MM == m_hwlayer_type &&
        is_support_mml &&
        is_game_hdr == false &&
        (Platform::getInstance().m_config.dbg_switch & HWC_DBG_SWITCH_DEBUG_MDP_AS_DISP_PQ) == 0)
    {
        m_layer_caps |= HWC_MML_OVL_LAYER;
    }

    if (usageHasProtectedOrSecure(getPrivateHandle().usage))
    {
        m_layer_caps |= HWC_LAYERING_OVL_ONLY;
    }

    switch (m_hwlayer_type)
    {
        case HWC_LAYER_TYPE_MM:
            if (isHint(HWC_LAYER_TYPE_MM) || // for debug
                isLayerPq()) // for layer pq
            {
                m_layer_caps |= HWC_LAYERING_OVL_ONLY;
            }
            break;
        case HWC_LAYER_TYPE_UI:
            if (isHint(HWC_LAYER_TYPE_UI)) // for debug
            {
                m_layer_caps |= HWC_LAYERING_OVL_ONLY;
            }
            break;
    }

    if (Platform::getInstance().m_config.is_client_clear_support)
    {
        if (m_hwlayer_type == HWC_LAYER_TYPE_INVALID)
            m_layer_caps &= ~HWC_CLIENT_CLEAR_LAYER;
        else if (m_hwlayer_type_file != HWC_COMP_FILE_HWC && !hasAlpha())
            m_layer_caps |= HWC_CLIENT_CLEAR_LAYER;
    }
}

void HWCLayer::setPerFrameMetadata(const std::map<int32_t, float>& per_frame_metadata)
{
    if (per_frame_metadata != m_per_frame_metadata)
    {
        m_per_frame_metadata = per_frame_metadata;
        setPerFrameMetadataChanged(true);

        // Setup hdr related metadata
        m_hdr_static_metadata_keys.clear();
        m_hdr_static_metadata_values.clear();
        m_hdr_static_metadata_keys.reserve(m_per_frame_metadata.size());
        m_hdr_static_metadata_values.reserve(m_per_frame_metadata.size());
        for (auto const& [key, val] : m_per_frame_metadata)
        {
            switch (key)
            {
                case HWC2_DISPLAY_RED_PRIMARY_X:
                case HWC2_DISPLAY_RED_PRIMARY_Y:
                case HWC2_DISPLAY_GREEN_PRIMARY_X:
                case HWC2_DISPLAY_GREEN_PRIMARY_Y:
                case HWC2_DISPLAY_BLUE_PRIMARY_X:
                case HWC2_DISPLAY_BLUE_PRIMARY_Y:
                case HWC2_WHITE_POINT_X:
                case HWC2_WHITE_POINT_Y:
                case HWC2_MAX_LUMINANCE:
                case HWC2_MIN_LUMINANCE:
                case HWC2_MAX_CONTENT_LIGHT_LEVEL:
                case HWC2_MAX_FRAME_AVERAGE_LIGHT_LEVEL:
                    m_hdr_static_metadata_keys.push_back(key);
                    m_hdr_static_metadata_values.push_back(val);
                    break;
                default:
                    break;
            }
        }
        uint32_t prev_hdr_type = m_hdr_type;
        m_hdr_type &= ~MTK_METADATA_TYPE_STATIC;
        if (!m_per_frame_metadata.empty())
        {
            m_hdr_type |= MTK_METADATA_TYPE_STATIC;
        }
        if (prev_hdr_type != m_hdr_type)
        {
            setStateChanged(HWC_LAYER_STATE_CHANGE_METADATA_TYPE);
        }
    }
    if (Platform::getInstance().m_config.dbg_switch & HWC_DBG_SWITCH_DEBUG_LAYER_METADATA)
    {
        DbgLogger logger(DbgLogger::TYPE_HWC_LOG, 'I', "(%" PRIu64 ":%" PRIu64 ") %s ", m_disp_id, getId(), __func__);
        for (auto const& [key, val] : m_per_frame_metadata)
        {
            logger.printf(",%d:%f", key, val);
        }
    }
}

void HWCLayer::setPerFrameMetadataBlobs(const std::map<int32_t, std::vector<uint8_t> >& per_frame_metadata_blobs)
{
    if (per_frame_metadata_blobs != m_per_frame_metadata_blobs)
    {
        m_per_frame_metadata_blobs = per_frame_metadata_blobs;
        setPerFrameMetadataChanged(true);

        // Setup hdr related metadata
        m_hdr_dynamic_metadata.clear();
        for (auto const& [key, val] : m_per_frame_metadata_blobs)
        {
            switch (static_cast<PerFrameMetadataKey>(key))
            {
                case PerFrameMetadataKey::HDR10_PLUS_SEI:
                    m_hdr_dynamic_metadata = val;
                    break;
                default:
                    break;
            }
        }
        uint32_t prev_hdr_type = m_hdr_type;
        m_hdr_type &= ~MTK_METADATA_TYPE_DYNAMIC;
        if (!m_per_frame_metadata_blobs.empty())
        {
            m_hdr_type |= MTK_METADATA_TYPE_DYNAMIC;
        }
        if (prev_hdr_type != m_hdr_type)
        {
            setStateChanged(HWC_LAYER_STATE_CHANGE_METADATA_TYPE);
        }
    }
    if (Platform::getInstance().m_config.dbg_switch & HWC_DBG_SWITCH_DEBUG_LAYER_METADATA)
    {
        DbgLogger logger(DbgLogger::TYPE_HWC_LOG, 'I', "(%" PRIu64 ":%" PRIu64 ") %s ", m_disp_id, getId(), __func__);
        for (auto const& [key, val] : m_per_frame_metadata_blobs)
        {
            logger.printf(",%d:", key);
            for(auto const& byte_value : val)
            {
                logger.printf(" %02x", byte_value);
            }
        }
    }
}

void HWCLayer::setPrevIsPQEnhance(const bool& val)
{
    m_prev_pq_enable = val;
}

bool HWCLayer::getPrevIsPQEnhance() const
{
    return m_prev_pq_enable;
}

bool HWCLayer::isNeedPQ(const int32_t& version) const
{
    if (!HwcFeatureList::getInstance().getFeature().game_pq)
    {
        return false;
    }

    if (version != 0 && version < HwcFeatureList::getInstance().getFeature().game_pq)
    {
        return false;
    }

    const PrivateHandle& priv_hnd = getPrivateHandle();
    if (priv_hnd.pq_info.value_size > 0)
    {
        return true;
    }

    return m_need_pq;
}

bool HWCLayer::isAIPQ() const
{
    return getPrivateHandle().ai_pq_info.param;
}

void HWCLayer::setBufferQueue(sp<DisplayBufferQueue> queue)
{
    m_queue = queue;
}

sp<DisplayBufferQueue> HWCLayer::getBufferQueue()
{
    return m_queue;
}

void HWCLayer::setupPrivateHandle()
{
    const PrivateHandle& prev_priv_hnd = getPrivateHandle();
    const unsigned int prevFormat = prev_priv_hnd.format_original;
    const uint32_t prevPrexForm = prev_priv_hnd.prexform;
    const bool prevSecure = usageHasProtectedOrSecure(prev_priv_hnd.usage) ||
                            (prev_priv_hnd.sec_handle != 0);

    bool init_layer_usage = m_name.empty();

    m_hwc_buf->setupPrivateHandle(&m_name);

    if (CC_UNLIKELY(init_layer_usage))
    {
        if (!m_name.empty())
        {
            HWC_LOGD("%s(), id %" PRIu64 ", %s", __FUNCTION__, m_id, m_name.c_str());
        }

        constexpr char dim_layer_name[] = "OnScreenFingerprintDimLayer";
        if (!strncmp(m_name.c_str(), dim_layer_name, strlen(dim_layer_name)))
        {
            m_layer_usage |= HWC_LAYER_USAGE_HBM;
            setStateChanged(HWC_LAYER_STATE_CHANGE_NAME);
        }
    }

    const PrivateHandle& priv_hnd = getPrivateHandle();

    // opaque RGBA layer can be processed as RGBX
    if (getBlend() == HWC2_BLEND_MODE_NONE &&
        priv_hnd.format == HAL_PIXEL_FORMAT_RGBA_8888)
    {
        getEditablePrivateHandle().format = HAL_PIXEL_FORMAT_RGBX_8888;
    }

    if (priv_hnd.format_original != prevFormat)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_FORMAT);
    }
    if (priv_hnd.prexform != prevPrexForm)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_PREXFORM);
    }
    if ((usageHasProtectedOrSecure(priv_hnd.usage) ||
        (priv_hnd.sec_handle != 0)) != prevSecure)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_SECURE);
    }

    uint32_t prev_hdr_type = m_hdr_type;
    m_hdr_type &= ~MTK_METADATA_TYPE_GRALLOC;
    if (priv_hnd.hdr_prop.is_hdr)
    {
        m_hdr_type |= MTK_METADATA_TYPE_GRALLOC;
    }
    if (prev_hdr_type != m_hdr_type)
    {
        setStateChanged(HWC_LAYER_STATE_CHANGE_METADATA_TYPE);
    }
}

void HWCLayer::boundaryCut(const hwc_rect_t& display_boundry)
{
    hwc_frect_t cut_source_crop;
    hwc_rect_t cut_display_frame;
    if (::boundaryCut(m_source_crop, m_display_frame, m_transform, display_boundry, &cut_source_crop, &cut_display_frame))
    {
        HWC_LOGD("(%" PRIu64 ") id:%" PRIu64 " crop xform=%d/Src(%.1f,%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f,%.1f)"
                 "/Dst(%d,%d,%d,%d)->(%d,%d,%d,%d)", m_disp_id, getId(), m_transform,
                 m_source_crop.left, m_source_crop.top, m_source_crop.right, m_source_crop.bottom,
                 cut_source_crop.left, cut_source_crop.top, cut_source_crop.right, cut_source_crop.bottom,
                 m_display_frame.left, m_display_frame.top, m_display_frame.right, m_display_frame.bottom,
                 cut_display_frame.left, cut_display_frame.top, cut_display_frame.right, cut_display_frame.bottom);
        m_source_crop = cut_source_crop;
        m_display_frame = cut_display_frame;
    }
}

bool HWCLayer::isUnchangedLayer()
{
    if (isBufferChanged() || !isStateUnchanged())
    {
        HWC_LOGV("isUnchangedLayer(), false- isBufferChanged:%d, isStateUnchanged:%d, reason 0x%x",
                isBufferChanged(), isStateUnchanged(), getStateChangedReason());
        return false;
    }

    // filter the special layers
    if (isAIPQ() || isNeedPQ(1)// || (getHdrScenario() != HWC_LAYER_HDR_NONE)
        || getPrivateHandle().pq_enable || (m_hwlayer_type != HWC_LAYER_TYPE_UI && m_hwlayer_type != HWC_LAYER_TYPE_INVALID)
        || (getSFCompositionType() == HWC2_COMPOSITION_CLIENT))// remove the conditon of needScaling()
    {
        HWC_LOGV("isUnchangedLayer(), false- filter the special layers");
        return false;
    }

    return true;
}

void HWCLayer::handleUnchangedLayer(uint64_t job_id)
{
    if (isUnchangedLayer())
    {
        m_unchanged_cnt++;
        HWC_LOGV("%s() job:%" PRIu64 " id:%" PRIu64 " ucnt:%d, unchanged_ratio_valid:%d",
            __func__, job_id, getId(), m_unchanged_cnt, m_unchanged_ratio_valid);
    }
    else
    {
        m_unchanged_cnt = 0;
        m_unchanged_ratio_valid = false;
        HWC_LOGV("%s() job:%" PRIu64 " id:%" PRIu64 " ucnt:%d", __func__, job_id, getId(), m_unchanged_cnt);
    }

}

bool HWCLayer::isInactiveLayer()
{
    if (isBufferChanged() || !isStateInactive())
    {
        HWC_LOGV("isInactiveLayer(), false- isBufferChanged:%d, isStateInactive:%d, reason 0x%x",
                isBufferChanged(), isStateInactive(), getStateChangedReason());
        return false;
    }

    // filter the special layers
    if (isAIPQ() || isNeedPQ(1)// || (getHdrScenario() != HWC_LAYER_HDR_NONE)
        || getPrivateHandle().pq_enable || (m_hwlayer_type != HWC_LAYER_TYPE_UI && m_hwlayer_type != HWC_LAYER_TYPE_INVALID)
        || (getSFCompositionType() == HWC2_COMPOSITION_CLIENT) || needScaling())
    {
        HWC_LOGV("isInactiveLayer(), false- filter the special layers");
        return false;
    }

    return true;

}

void HWCLayer::handleInactiveLayer(uint64_t job_id)
{
    if (isInactiveLayer())
    {
        m_inactive_cnt++;
        HWC_LOGV("%s() job:%" PRIu64 " id:%" PRIu64 " icnt:%d inactive_set_hint:%d is_inactive_client_layer:%d fbt_unchanged_hint:%d",
            __func__, job_id, getId(), m_inactive_cnt, m_inactive_set_hint, m_is_inactive_client_layer, m_fbt_unchanged_hint);
    }
    else
    {
        m_inactive_cnt = 0;
        HWC_LOGV("%s() job:%" PRIu64 " id:%" PRIu64 " icnt:%d",
            __func__, job_id, getId(), m_inactive_cnt);
    }
}

void HWCLayer::initPrevHwlayerType(const int32_t& hwlayer_type, const int32_t& line, const HWC_COMP_FILE& file)
{
    m_prev_hwlayer_type = hwlayer_type;
    m_prev_hwlayer_type_line = line;
    m_prev_hwlayer_type_file = file;
}

void HWCLayer::setBlockingRegion(hwc_region_t blocking)
{
    if (!isHwcRegionEqual(m_blocking_region, blocking))
    {
        copyHwcRegion(&m_blocking_region, blocking);
    }
}

int32_t HWCLayer::setBrightness(float brightness)
{
    if (brightness > 1.0f || brightness < 0.0f || std::isnan(brightness))
    {
        HWC_LOGW("set layer brightness with invalid value: %f", brightness);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    if (m_brightness != brightness)
    {
        m_brightness = brightness;
    }
    return HWC2_ERROR_NONE;
}

void HWCLayer::setColorTransform(const float* matrix)
{
    //TODO the matrix hint is not correct. doo we care it? gieve the correct hint when implement
    //     this function.
    m_color_transform = new ColorTransform(matrix, HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX, true);
}

//-----------------------------------------------------------------------------

HWCPresentIdxLayer::HWCPresentIdxLayer(const wp<HWCDisplay>& disp, const uint64_t& disp_id,
        const uint32_t width, const uint32_t height, const uint32_t x, const uint32_t y)
    : HWCLayer(disp, disp_id, false)
{
    initialState(width, height, x, y);
}

HWCPresentIdxLayer::~HWCPresentIdxLayer()
{
}

void HWCPresentIdxLayer::setReleaseFenceFd(const int32_t& fence_fd, const bool& /*is_disp_connected*/)
{
    ::protectedClose(fence_fd);
}

void HWCPresentIdxLayer::initialState(const uint32_t width, const uint32_t height,
        const uint32_t x, const uint32_t y)
{
    setStateChanged(HWC_LAYER_STATE_CHANGE_SF_COMP_TYPE);
    setHwlayerType(HWC_LAYER_TYPE_UI, __LINE__, HWC_COMP_FILE_HWC);
    setSFCompositionType(HWC2_COMPOSITION_DEVICE);

    GrallocDevice::AllocParam param;
    param.width  = width;
    param.height = height;
    param.format = HAL_PIXEL_FORMAT_RGBA_8888;
    param.usage  = static_cast<uint64_t>(BufferUsage::COMPOSER_OVERLAY);
    param.usage |= BufferUsage::CPU_READ_OFTEN | BufferUsage::CPU_WRITE_OFTEN;
    if (NO_ERROR != GrallocDevice::getInstance().alloc(param))
    {
        HWC_LOGE("Failed to allocate memory size(w=%d,h=%d,fmt=%d,usage=%" PRIx64 ")",
            param.width, param.height, param.format, param.usage);
        return;
    }
    buffer_handle_t handle = param.handle;

    PrivateHandle ph;
    ::getPrivateHandle(handle, &ph);
    size_t sz = static_cast<size_t>(ph.size);
    HWC_LOGI("try to map buffer for debug layer(%zu,%d)(%ux%u)", sz, ph.size, ph.y_stride, ph.vstride);
    void* ptr = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, ph.ion_fd, 0);
    //TODO draw something in here
    if (ptr)
    {
        memset(ptr, 255, sz);
        munmap(ptr, sz);
    }
    else
    {
        HWC_LOGE("failed to map buffer for debug layer");
    }

    setHandle(handle);
    setupPrivateHandle();
    setBlend(HWC2_BLEND_MODE_PREMULTIPLIED);
    hwc_rect_t disp_frame = {.left = static_cast<int>(x), .right = static_cast<int>(x + width),
            .top = static_cast<int>(y), .bottom = static_cast<int>(y + height)};
    setDisplayFrame(disp_frame);
    hwc_frect_t src_crop = {.left = 0.0f, .right = static_cast<float>(width), .top = 0.0f,
            .bottom = static_cast<float>(height)};
    setSourceCrop(src_crop);
    setZOrder(UINT32_MAX);
    setPlaneAlpha(1.0f);
    m_debug_type = HWC_DEBUG_LAYER_TYPE_PRESENT_IDX;
}
