#ifndef HWC_HWCLAYER_H
#define HWC_HWCLAYER_H

#include <vector>
#include <map>

#include <utils/RefBase.h>
#include "utils/tools.h"

#include "hwc2_defs.h"
#include "hwcbuffer.h"

enum {
    HWC_LAYER_FLAG_FORCE_RGBX = 1 << 0,
};

enum {
    HWC_LAYER_STATE_CHANGE_DATASPACE = 1 << 0,
    HWC_LAYER_STATE_CHANGE_BLEND = 1 << 1,
    HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_OFFSET = 1 << 2,
    HWC_LAYER_STATE_CHANGE_SOURCE_CROP = 1 << 3,
    HWC_LAYER_STATE_CHANGE_PLANE_ALPHA = 1 << 4,
    HWC_LAYER_STATE_CHANGE_ZORDER = 1 << 5,
    HWC_LAYER_STATE_CHANGE_TRANSFORM = 1 << 6,
    HWC_LAYER_STATE_CHANGE_VISIBLE_REGION = 1 << 7,
    HWC_LAYER_STATE_CHANGE_COLOR = 1 << 8,
    HWC_LAYER_STATE_CHANGE_METADATA = 1 << 9,
    HWC_LAYER_STATE_CHANGE_SF_COMP_TYPE = 1 << 10,
    HWC_LAYER_STATE_CHANGE_FORCE_RGBX = 1 << 11,
    HWC_LAYER_STATE_CHANGE_FORMAT = 1 << 12,
    HWC_LAYER_STATE_CHANGE_PREXFORM = 1 << 13,
    HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_SIZE = 1 << 14,
    HWC_LAYER_STATE_CHANGE_SECURE = 1 << 15,
    HWC_LAYER_STATE_CHANGE_NAME = 1 << 16,

    // add here if state change affects layer dirty
    HWC_LAYER_STATE_CHANGE_DIRTY = HWC_LAYER_STATE_CHANGE_DATASPACE |
                                   HWC_LAYER_STATE_CHANGE_BLEND |
                                   HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_OFFSET |
                                   HWC_LAYER_STATE_CHANGE_SOURCE_CROP |
                                   HWC_LAYER_STATE_CHANGE_PLANE_ALPHA |
                                   HWC_LAYER_STATE_CHANGE_TRANSFORM |
                                   HWC_LAYER_STATE_CHANGE_VISIBLE_REGION |
                                   HWC_LAYER_STATE_CHANGE_COLOR |
                                   HWC_LAYER_STATE_CHANGE_METADATA |
                                   HWC_LAYER_STATE_CHANGE_SF_COMP_TYPE |
                                   HWC_LAYER_STATE_CHANGE_FORCE_RGBX |
                                   HWC_LAYER_STATE_CHANGE_FORMAT |
                                   HWC_LAYER_STATE_CHANGE_PREXFORM |
                                   HWC_LAYER_STATE_CHANGE_DISPLAY_FRAME_SIZE |
                                   HWC_LAYER_STATE_CHANGE_SECURE,
};

enum {
    HWC_LAYER_USAGE_NONE = 0,
    HWC_LAYER_USAGE_HBM = 1 << 0,
    HWC_LAYER_USAGE_AOD = 1 << 1,
};

enum : uint32_t
{
    MTK_METADATA_TYPE_NONE = 0,
    MTK_METADATA_TYPE_GRALLOC = 1 << 0,
    MTK_METADATA_TYPE_STATIC = 1 << 1,
    MTK_METADATA_TYPE_DYNAMIC = 1 << 2,
};

class HWCBuffer;
class DisplayBufferQueue;

class HWCLayer : public android::LightRefBase<HWCLayer>
{
public:
    static std::atomic<uint64_t> id_count;

    HWCLayer(const wp<HWCDisplay>& disp, const uint64_t& disp_id, const bool& is_ct);
    ~HWCLayer();

    uint64_t getId() const { return m_id; };

    bool isClientTarget() const { return m_is_ct; }

    wp<HWCDisplay> getDisplay() const { return m_disp; }

    void validate(int32_t pq_mode_id);
    int32_t afterPresent(const bool& should_clear_state = true);

    void setHwlayerType(const int32_t& hwlayer_type, const int32_t& line, const HWC_COMP_FILE& file);

    bool decideMdpOutputCompressedBuffers() const;
    uint32_t decideMdpOutputFormat() const;

    int32_t getHwlayerType() const { return m_hwlayer_type; }
    int32_t getHwlayerTypeLine() const { return m_hwlayer_type_line; }
    HWC_COMP_FILE getHwlayerTypeFile() const { return m_hwlayer_type_file; }

    int32_t getCompositionType() const;

    void setSFCompositionType(const int32_t& sf_comp_type, const bool& call_from_sf);
    int32_t getSFCompositionType() const { return m_sf_comp_type; }
    int32_t getLastCompTypeCallFromSF() const { return m_last_comp_type_call_from_sf; }
    bool isSFCompositionTypeCallFromSF() const { return m_sf_comp_type_call_from_sf; }

    void setHandle(const buffer_handle_t& hnd);
    buffer_handle_t getHandle() { return m_hwc_buf->getHandle(); }

    const PrivateHandle& getPrivateHandle() const { return m_hwc_buf->getPrivateHandle(); }

    PrivateHandle& getEditablePrivateHandle() { return m_hwc_buf->getEditablePrivateHandle(); }

    void setReleaseFenceFd(const int32_t& fence_fd, const bool& is_disp_connected);
    int32_t getReleaseFenceFd() { return m_hwc_buf->getReleaseFenceFd(); }

    void setPrevReleaseFenceFd(const int32_t& fence_fd, const bool& is_disp_connected);
    int32_t getPrevReleaseFenceFd() { return m_hwc_buf->getPrevReleaseFenceFd(); }

    void setAcquireFenceFd(const int32_t& acquire_fence_fd);
    int32_t getAcquireFenceFd() { return m_hwc_buf->getAcquireFenceFd(); }

    void setDataspace(const int32_t& dataspace);
    int32_t getDataspace() const;
    int32_t decideMdpOutDataspace() const;

    void setDamage(const hwc_region_t& damage);
    const hwc_region_t& getDamage() { return m_damage; }

    void setBlend(const int32_t& blend);
    int32_t getBlend() const { return m_blend; }

    void setDisplayFrame(const hwc_rect_t& display_frame);
    const hwc_rect_t& getDisplayFrame() const { return m_display_frame; }

    void setSourceCrop(const hwc_frect_t& source_crop);
    const hwc_frect_t& getSourceCrop() const { return m_source_crop; }

    void setZOrder(const uint32_t& z_order);
    uint32_t getZOrder() { return m_z_order; }

    void setPlaneAlpha(const float& plane_alpha);
    float getPlaneAlpha() { return m_plane_alpha; }

    void setTransform(const unsigned int& transform);
    unsigned int getTransform() const { return m_transform; }

    void setVisibleRegion(const hwc_region_t& visible_region);
    const hwc_region_t& getVisibleRegion() { return m_visible_region; }

    void setBufferChanged(const bool& changed) { return m_hwc_buf->setBufferChanged(changed); }
    bool isBufferChanged() const { return m_hwc_buf->isBufferChanged(); }

    void setStateChanged(const int& changed) { m_state_changed |= changed; }
    void clearStateChanged() { m_state_changed = 0; }
    bool isStateChanged() const { return m_state_changed != 0; }
    int getStateChangedReason() { return m_state_changed; }

    bool isStateContentDirty() const { return (m_state_changed & HWC_LAYER_STATE_CHANGE_DIRTY) != 0; }

    void setForcePQ(const bool& is_force_pq) { m_need_pq = is_force_pq; }

    // @param version:
    //    0 means all version
    //    1 means AppGamePQ 1.0
    //    2 means AppGamePQ 2.0
    bool isNeedPQ(const int32_t& version = 0) const;
    bool isAIPQ() const;

    void setMtkFlags(const int64_t& mtk_flags) {m_mtk_flags = mtk_flags; }
    int64_t getMtkFlags() const { return m_mtk_flags; }

    void setLayerColor(const hwc_color_t& color);
    uint32_t getLayerColor() { return m_layer_color; }

    const hwc_rect_t& getMdpDstRoi() const { return m_mdp_dst_roi; }
    hwc_rect_t& editMdpDstRoi() { return m_mdp_dst_roi; }

    sp<HWCBuffer> getHwcBuffer() { return m_hwc_buf; }

    void toBeDim();

    void setupPrivateHandle();

    void setVisible(const bool& is_visible) { m_is_visible = is_visible; }
    bool isVisible() const { return m_is_visible; }

    String8 toString8();

    // return final transform rectify with prexform
    uint32_t getXform() const;
    bool needRotate() const;
    bool needScaling() const;

    void initValidate();
    void setLayerCaps(const int32_t& layer_caps) { m_layer_caps = layer_caps; }
    int32_t getLayerCaps() const { return m_layer_caps; }
    void completeLayerCaps(bool bottom);

    void setPerFrameMetadata(const std::map<int32_t, float>& per_frame_metadata);
    const std::map<int32_t, float>& getPerFrameMetadata() const { return m_per_frame_metadata; }

    void setPerFrameMetadataBlobs(const std::map<int32_t, std::vector<uint8_t> >& per_frame_metadata_blobs);
    const std::map<int32_t, std::vector<uint8_t> >& getPerFrameMetadataBlobs() const { return m_per_frame_metadata_blobs; }

    const std::vector<int32_t>& getHdrStaticMetadataKeys() const { return m_hdr_static_metadata_keys; }
    const std::vector<float>& getHdrStaticMetadataValues() const { return m_hdr_static_metadata_values; }
    const std::vector<uint8_t>& getHdrDynamicMetadata() const { return m_hdr_dynamic_metadata; }

    void setPrevIsPQEnhance(const bool& val);
    bool getPrevIsPQEnhance() const;

    void setOverrideMDPOutputFormat(const unsigned int & val) { m_is_override_MDP_output_format = val; }
    unsigned int  getOverrideMDPOutputFormat() const { return m_is_override_MDP_output_format; }

    void setLastAppGamePQ(const bool& app_game_pq) { m_last_app_game_pq = app_game_pq; }
    bool getLastAppGamePQ() const { return m_last_app_game_pq; }

    void setLastAIPQ(const bool& on) { m_last_ai_pq = on; }
    bool getLastAIPQ() const { return m_last_ai_pq; }

    void setLastCameraPreviewHDR(const bool& on) { m_last_camera_preview_hdr = on; }
    bool getLastCameraPreviewHDR() const { return m_last_camera_preview_hdr; }

    void setBufferQueue(sp<DisplayBufferQueue> queue);
    sp<DisplayBufferQueue> getBufferQueue();

    void setPerfState(const bool& has_set_perf) { m_has_set_perf = has_set_perf; }
    bool getPerfState() const { return m_has_set_perf; }

    bool hasAlpha() const;

    bool isPixelAlphaUsed() const;

    int32_t getHWCRequests() const { return m_hwc_requests; }
    void setHWCRequests(const int32_t& hwc_requests) { m_hwc_requests = hwc_requests; }

    bool isGameHDR() const;

    bool isCameraPreviewHDR() const;

    bool isHint(int32_t hint_hwlayer_type) const;

    void boundaryCut(const hwc_rect_t& display_boundry);

    std::string getName() const { return m_name; }

    int getLayerUsage() const { return m_layer_usage; }

    mml_frame_info* getMMLCfg() { return &m_mml_cfg; }

    uint32_t getHdrType() const { return m_hdr_type; }

    void setGlaiAgentId(const int agent_id) { m_glai_agent_id = agent_id; }
    int getGlaiAgentId() { return m_glai_agent_id; }
    const hwc_rect_t& getGlaiDstRoi() const { return m_glai_dst_roi; }
    hwc_rect_t& editGlaiDstRoi() { return m_glai_dst_roi; }
    void setGlaiOutFormat(unsigned int format) { m_glai_out_format = format; }
    unsigned int getGlaiOutFormat() { return m_glai_out_format; }
    void setGlaiLastInference(const bool& on) { m_glai_last_inference = on; }
    bool getGlaiLastInference() const { return m_glai_last_inference; }

private:
    int64_t m_mtk_flags;

    uint64_t m_id;

    const bool m_is_ct;

    wp<HWCDisplay> m_disp;

    int32_t m_hwlayer_type;
    int32_t m_hwlayer_type_line;
    HWC_COMP_FILE m_hwlayer_type_file;

    int32_t m_sf_comp_type;

    int32_t m_dataspace;

    hwc_region_t m_damage;

    int32_t m_blend;

    hwc_rect_t m_display_frame;

    hwc_frect_t m_source_crop;

    float m_plane_alpha;

    uint32_t m_z_order;

    unsigned int m_transform;

    hwc_region_t m_visible_region;

    int m_state_changed;

    PrivateHandle m_priv_hnd;

    hwc_rect_t m_mdp_dst_roi;

    hwc_rect_t m_glai_dst_roi;

    uint64_t m_disp_id;

    sp<HWCBuffer> m_hwc_buf;

    bool m_is_visible;

    bool m_sf_comp_type_call_from_sf;

    int32_t m_last_comp_type_call_from_sf;

    int32_t m_layer_caps;

    uint32_t m_layer_color;

    std::map<int32_t, float> m_per_frame_metadata;
    std::map<int32_t, std::vector<uint8_t> > m_per_frame_metadata_blobs;
    std::vector<int32_t> m_hdr_static_metadata_keys;
    std::vector<float> m_hdr_static_metadata_values;
    std::vector<uint8_t> m_hdr_dynamic_metadata;
    uint32_t m_hdr_type;

    bool m_prev_pq_enable;

    unsigned int m_is_override_MDP_output_format;

    bool m_need_pq;

    bool m_last_app_game_pq;

    bool m_last_ai_pq;

    bool m_last_camera_preview_hdr;

    sp<DisplayBufferQueue> m_queue;

    bool m_has_set_perf;

    int32_t m_hwc_requests;

    std::string m_name;

    int m_layer_usage;

    mml_frame_info m_mml_cfg;

    int m_glai_agent_id; // > 0 means have model in this hwc layer
    unsigned int m_glai_out_format;
    bool m_glai_last_inference;
};

#endif
