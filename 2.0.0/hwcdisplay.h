#ifndef HWC_HWCDISPLAY_H
#define HWC_HWCDISPLAY_H

#include <vector>
#include <utils/RefBase.h>
#include <hardware/hwcomposer2.h>

#include "utils/tools.h"
#include "utils/fpscounter.h"

#include "hwcbuffer.h"
#include "hwclayer.h"
#include "hwc2_defs.h"
#include "color.h"
#include "color_histogram.h"

struct IntentInfo
{
    int32_t id;
    int32_t dynamic_range;
    int32_t intent;
};

enum
{
    MTK_COMPOSITION_MODE_INIT,
    MTK_COMPOSITION_MODE_NORMAL,
    MTK_COMPOSITION_MODE_DECOUPLE,
};

class HWCDisplay : public RefBase
{
public:
    HWCDisplay(const uint64_t& disp_id, const int32_t& type, const sp<IOverlayDevice>& ovl);

    void init();

    void validate();
    void beforePresent(const unsigned int num_plugin_display);

    void present();

    void afterPresent();

    void clear();

    bool isConnected() const;

    bool isValidated() const;

    void getChangedCompositionTypes(
        uint32_t* out_num_elem,
        hwc2_layer_t* out_layers,
        int32_t* out_types) const;

    void getCompositionMode(bool& has_changed, int& new_mode);

    sp<HWCLayer> getLayer(const hwc2_layer_t& layer_id);

    const std::vector<sp<HWCLayer> >& getVisibleLayersSortedByZ();
    const std::vector<sp<HWCLayer> >& getInvisibleLayersSortedByZ();
    const std::vector<sp<HWCLayer> >& getCommittedLayers();

    sp<HWCLayer> getClientTarget();

    int32_t getWidth(hwc2_config_t config = 0) const;
    int32_t getHeight(hwc2_config_t config = 0) const;
    nsecs_t getVsyncPeriod(hwc2_config_t config = 0) const;
    int32_t getDpiX(hwc2_config_t config = 0) const;
    int32_t getDpiY(hwc2_config_t config = 0) const;
    int32_t getConfigGroup(hwc2_config_t config = 0) const;
    uint32_t getNumConfigs() const;
    bool setActiveConfig(hwc2_config_t config);
    hwc2_config_t getActiveConfig() { return m_active_config; }
    bool isConfigChanged();
    int32_t getSecure() const;

    void setPowerMode(const int32_t& mode);
    int32_t getPowerMode() { return m_power_mode; }

    void setVsyncEnabled(const int32_t& enabled);

    void getType(int32_t* out_type) const;

    uint64_t getId() const { return m_disp_id; }

    int32_t createLayer(hwc2_layer_t* out_layer, const bool& is_ct);
    int32_t destroyLayer(const hwc2_layer_t& layer);

    int32_t getMirrorSrc() const { return m_mir_src; }
    void setMirrorSrc(const int32_t& disp) { m_mir_src = disp; }

    void getGlesRange(int32_t* gles_head, int32_t* gles_tail) const
    {
        *gles_head = m_gles_head;
        *gles_tail = m_gles_tail;
    }

    void setGlesRange(const int32_t& gles_head, const int32_t& gles_tail);

    void updateGlesRange();

    void acceptChanges();

    int32_t getRetireFenceFd() const {
        return m_retire_fence_fd;
    }

    void setRetireFenceFd(const int32_t& retire_fence_fd, const bool& is_disp_connected);

    void clearAllFences();

    const std::vector<int32_t>& getPrevCompTypes() { return m_prev_comp_types; }

    void moveChangedCompTypes(std::vector<sp<HWCLayer> >* changed_comp_types)
    {
        // todo: move ?
        m_changed_comp_types = *changed_comp_types;
    }

    void getReleaseFenceFds(
        uint32_t* out_num_elem,
        hwc2_layer_t* out_layer,
        int32_t* out_fence_fd);

    void getClientTargetSupport(
        const uint32_t& width, const uint32_t& height,
        const int32_t& format, const int32_t& dataspace);

    void setOutbuf(const buffer_handle_t& handle, const int32_t& release_fence_fd);
    sp<HWCBuffer> getOutbuf() { return m_outbuf;}

    void dump(String8* dump_str);

    void initPrevCompTypes();

    void buildVisibleAndInvisibleLayersSortedByZ();
    void buildCommittedLayers();

    int32_t getColorTransformHint() { return m_color_transform_hint; }
    int32_t setColorTransform(const float* matrix, const int32_t& hint);

    int32_t getColorMode() { return m_color_mode; }
    int32_t setColorMode(const int32_t& color_mode);

    bool needDisableVsyncOffset() { return m_need_av_grouping; }

    void setupPrivateHandleOfLayers();

    const std::vector<sp<HWCLayer> >& getLastCommittedLayers() const { return m_last_committed_layers; }
    void setLastCommittedLayers(const std::vector<sp<HWCLayer> >& last_committed_layers) { m_last_committed_layers = last_committed_layers; }

    bool isGeometryChanged()
    {
#ifndef MTK_USER_BUILD
        ATRACE_CALL();
#endif
        const auto& committed_layers = getCommittedLayers();
        const auto& last_committed_layers = getLastCommittedLayers();
        if (committed_layers != last_committed_layers)
            return true;

        bool is_dirty = false;

        for (auto& layer: committed_layers)
        {
            is_dirty |= layer->isStateChanged();
        }
        return is_dirty;
    }

    void removePendingRemovedLayers();
    void buildVisibleAndInvisibleLayer();

    bool isValid()
    {
        if (!isConnected() || getPowerMode() == HWC2_POWER_MODE_OFF)
            return false;

        return true;
    }

    void setJobDisplayOrientation();
    void setJobDisplayData();
    bool isForceGpuCompose();

    unsigned int getPrevAvailableInputLayerNum() const { return m_prev_available_input_layer_num; }
    void setPrevAvailableInputLayerNum(unsigned int availInputNum) { m_prev_available_input_layer_num = availInputNum; }

    void setValiPresentState(HWC_VALI_PRESENT_STATE val, const int32_t& line);

    HWC_VALI_PRESENT_STATE getValiPresentState() const { return m_vali_present_state; }

    bool isVisibleLayerChanged() const { return m_is_visible_layer_changed; }
    void checkVisibleLayerChange(const std::vector<sp<HWCLayer> > &prev_visible_layers);
    void setColorTransformForJob(DispatcherJob* const job);

    void setOverrideMDPOutputFormatOfLayers();
    void setJobVideoTimeStamp(DispatcherJob* job);

    bool isGpuComposition() const { return m_use_gpu_composition; }
    bool isColorTransformOK() const { return m_color_transform_ok; }
    void setGpuComposition(bool enable) { m_use_gpu_composition = enable; }

    void addUnpresentCount();
    void decUnpresentCount();
    int getUnpresentCount() { return m_unpresent_count; }
    int getPrevUnpresentCount() { return m_prev_unpresent_count; }
#ifdef MTK_IN_DISPLAY_FINGERPRINT
    // for LED HBM (High Backlight Mode) control
    bool getIsHBM() const { return m_is_HBM; }
    void setIsHBM(const bool& val) { m_is_HBM = val; }
#endif

    void updateGlesRangeHwType();
    void moveChangedHWCRequests(std::vector<sp<HWCLayer> >* changed_hwc_requests)
    {
        m_changed_hwc_requests = *changed_hwc_requests;
    }
    void getChangedRequests(
        uint32_t* out_num_elem,
        hwc2_layer_t* out_layers,
        int32_t* out_requests) const;
    unsigned int getClientClearLayerNum() { return m_client_clear_layer_num; };

    void setJobStatus(const int& dispatcher_job_status) { m_dispathcer_job_status = dispatcher_job_status; }
    int getJobStatus() { return m_dispathcer_job_status; }

    void setMSync2Enable(bool enable);
    void setMSync2ParamTable(std::shared_ptr<MSync2Data::ParamTable> param_table)
    {
        m_msync_2_0_param_table = param_table;
    }

    // color histogram function
    // initalize the color histogram
    void initColorHistogram(const sp<IOverlayDevice>& ovl);

    // get the capability of color histogram
    int32_t getContentSamplingAttribute(int32_t* format, int32_t* dataspace, uint8_t* mask);

    // enable or disable color histogram
    int32_t setContentSamplingEnabled(const int32_t enable, const uint8_t component_mask,
            const uint64_t max_frams);

    // get the result of color histogram
    int32_t getContentSample(const uint64_t max_frames, const uint64_t timestamp,
            uint64_t* frame_count, int32_t samples_size[4], uint64_t* samples[4]);

    // when PostHandler get a present fence, set the present fence to color histogram
    int32_t addPresentInfo(unsigned int index, int fd, hwc2_config_t active_config);

    // color mode with render intent
    void populateColorModeAndRenderIntent(const sp<IOverlayDevice>& ovl);
    int32_t getColorModeList(uint32_t* out_num_modes, int32_t* out_modes);
    int32_t getRenderIntents(const int32_t mode, uint32_t* out_num_intents, int32_t* out_intents);
    int32_t setColorModeWithRenderIntent(const int32_t mode, const int32_t intent);

    void setSfTargetTs(nsecs_t ts) { m_sf_target_ts = ts; }

    void calculatePerf(DispatcherJob* job);

    // follow the scenario or platform switch to choose cpu set
    void setScenarioHint(ComposerExt::ScenarioHint flag, ComposerExt::ScenarioHint mask);
    unsigned int updateCpuSet();
    void setDirtyCpuSet();

private:
    bool needDoAvGrouping(const unsigned int num_plugin_display);
    void updateFps();
    void updateLayerPrevInfo();
    void offloadMMtoClient();

    void setLastAppGamePQ(const bool& on) { m_last_app_game_pq = on; }
    bool getLastAppGamePQ() const { return m_last_app_game_pq; }

    void updatePqModeId(bool force_update = false);

    // if this frame has video or camera buffer, this function extends surfaceflinger target time.
    // then mdp and mml can get more time to process buffer.
    void extendSfTargetTime(DispatcherJob* job);

    int32_t m_type;
    sp<HWCBuffer> m_outbuf;

    bool m_is_validated;
    std::vector<sp<HWCLayer> > m_changed_comp_types;

    uint64_t m_disp_id;
    hwc2_config_t m_active_config;
    bool m_config_changed;

    std::map<uint64_t, sp<HWCLayer> > m_layers;
    mutable Mutex m_pending_removed_layers_mutex;
    mutable Mutex m_dump_lock;
    std::set<uint64_t> m_pending_removed_layers_id;
    std::vector<sp<HWCLayer> > m_visible_layers;
    std::vector<sp<HWCLayer> > m_invisible_layers;
    std::vector<sp<HWCLayer> > m_committed_layers;

    // client target
    sp<HWCLayer> m_ct;

    int32_t m_gles_head;
    int32_t m_gles_tail;
    int32_t m_retire_fence_fd;
    int32_t m_mir_src;
    std::vector<int32_t> m_prev_comp_types;
    int32_t m_power_mode;
    int32_t m_color_transform_hint;
    int32_t m_color_mode;
    int32_t m_prev_color_mode;
    int32_t m_render_intent;
    int32_t m_prev_render_intent;
    uint32_t m_hdr_type;
    uint32_t m_prev_hdr_type;
    int32_t m_presented_pq_mode_id;
    std::atomic<int32_t> m_pq_mode_id;

    bool m_need_av_grouping;
    bool m_use_gpu_composition;

    std::vector<sp<HWCLayer> > m_last_committed_layers;
    bool m_color_transform_ok;
    sp<ColorTransform> m_color_transform;
    CCORR_STATE m_ccorr_state;
    unsigned int m_prev_available_input_layer_num;

    HWC_VALI_PRESENT_STATE m_vali_present_state;
    bool m_is_visible_layer_changed;

    FpsCounter mFpsCounter;

    // display ID map to mirror count
    int m_unpresent_count;
    int m_prev_unpresent_count;
#ifdef MTK_IN_DISPLAY_FINGERPRINT
    // for LED HBM (High Backlight Mode) control
    bool m_is_HBM;
#endif

    unsigned int m_client_clear_layer_num;
    std::vector<sp<HWCLayer> > m_changed_hwc_requests;

    // mark having job for this run, this will hint whether
    // to clear layer state change and buffer change
    int m_dispathcer_job_status;

    bool m_last_app_game_pq;

    bool m_is_m4u_sec_inited;

    bool m_msync_2_0_enable_changed = false;
    int m_msync_2_0_enable = false;
    std::shared_ptr<MSync2Data::ParamTable> m_msync_2_0_param_table;

    std::shared_ptr<ColorHistogram> m_histogram;

    // the mapping table for PQ XML's mode id
    std::map<int32_t, std::map<int32_t, std::vector<IntentInfo>>> m_color_mode_with_render_intent;

    nsecs_t m_sf_target_ts = -1;
    nsecs_t m_prev_sf_target_ts = -1;   // check sf set ts every present
    std::string m_perf_ovl_atomic_work_time_str;

    bool m_need_free_fb_cache = false;

    // when we want to update the state of cpu set, set this flag to true
    bool m_dirty_cpu_set = false;
    // store the last ScenarioHint from other module
    ComposerExt::ScenarioHint m_scenario_hint = ComposerExt::ScenarioHint::kGeneral;
    // store the last cpu set after clear cpu set dirty
    unsigned int m_cpu_set = HWC_CPUSET_NONE;

    int m_prev_composition_mode = MTK_COMPOSITION_MODE_INIT;

    // the string of trace for extening surfaceflinger target time
    std::string m_perf_extend_sf_target_time_str;
    // store the number of visable video layer
    unsigned int m_num_video_layer;
    // store the number of visable camera layer
    unsigned int m_num_camera_layer;
    // store the previous extending sf target time
    nsecs_t m_prev_extend_sf_target_time;
};

#endif
