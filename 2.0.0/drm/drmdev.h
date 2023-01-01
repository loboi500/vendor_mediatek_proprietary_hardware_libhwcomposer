#ifndef DRM_HWDEV_H_
#define DRM_HWDEV_H_

#include <stdint.h>
#include <thread>

#include <linux/mediatek_drm.h>

#include "dev_interface.h"
#include "drm/drmmoderesource.h"
#include "drm/drmmodeutils.h"
#include <mtk-mml.h>
#include "drm/drmhistogram.h"

using namespace android;

class FrameCfg
{
public:
    FrameCfg();
    ~FrameCfg() {}

    unsigned int session_id;
    unsigned int mode;
};

enum
{
    DISPLAY_STATE_INACTIVE,
    DISPLAY_STATE_WAIT_TO_CREATE,
    DISPLAY_STATE_ACTIVE,
};

enum
{
    MTK_DRM_INVALID_SESSION_ID = static_cast<unsigned int>(-1),
};

class DisplayState
{
public:
    DisplayState();
    ~DisplayState();

    int checkDisplayStateMachine(uint64_t dpy, int new_state);
    int changeDisplayState(uint64_t dpy, int new_state);
    int getState(uint64_t dpy) { return m_state[dpy]; }
    unsigned int getNumberActiveDisplay() { return m_num_active_display; }
    unsigned int getNumberWaitedDisplay() { return m_num_waited_display; }

private:
    int m_state[DisplayManager::MAX_DISPLAYS];
    unsigned int m_num_active_display;
    unsigned int m_num_waited_display;
};

class DrmDevice : public IOverlayDevice
{
public:
    static DrmDevice& getInstance();
    ~DrmDevice();

    int32_t getType() { return OVL_DEVICE_TYPE_OVL; }

    // initOverlay() initializes overlay related hw setting
    void initOverlay();

    // getHwVersion() is used to get the HW version for check platform family
    static unsigned int getHwVersion();

    // getDisplayRotation gets LCM's degree
    uint32_t getDisplayRotation(uint64_t dpy);

    // isDispRszSupported() is used to query if display rsz is supported
    bool isDispRszSupported();

    // isDispRszSupported() is used to query if display rsz is supported
    bool isDispRpoSupported();

    // isPartialUpdateSupported() is used to query if PartialUpdate is supported
    bool isPartialUpdateSupported();

    // isFenceWaitSupported() is used to query if FenceWait is supported
    bool isFenceWaitSupported();

    // isConstantAlphaForRGBASupported is used to query if hardware support constant alpha for RGBA
    bool isConstantAlphaForRGBASupported();

    // isDispSelfRefreshSupported is used to query if hardware support ioctl of self-refresh
    bool isDispSelfRefreshSupported();

    // isDisplayHrtSupport() is used to query HRT suuported or not
    bool isDisplayHrtSupport();

    // isDisplaySupportedWidthAndHeight() is used to query Width and Height is supported or not
    bool isDisplaySupportedWidthAndHeight(unsigned int width, unsigned int height);

    // isMMLPrimarySupported() is used to query primary display is support MML or not
    bool isMMLPrimarySupported();

    // getMaxOverlayInputNum() gets overlay supported input amount
    unsigned int getMaxOverlayInputNum();

    // createOverlaySession() creates overlay composition session
    status_t createOverlaySession(
        uint64_t dpy, uint32_t width, uint32_t height,
        HWC_DISP_MODE mode = HWC_DISP_SESSION_DIRECT_LINK_MODE);

    // destroyOverlaySession() destroys overlay composition session
    void destroyOverlaySession(uint64_t dpy);

    // truggerOverlaySession() used to trigger overlay engine to do composition
    status_t triggerOverlaySession(uint64_t dpy, int present_fence_idx, int sf_present_fence_idx,
                                   int ovlp_layer_num, int prev_present_fence_fd, hwc2_config_t config,
                                   const uint32_t& hrt_weight, const uint32_t& hrt_idx,
                                   unsigned int num, OverlayPortParam* const* params,
                                   sp<ColorTransform> color_transform,
                                   TriggerOverlayParam trigger_param
                                   );

    // disableOverlaySession() usd to disable overlay session to do composition
    void disableOverlaySession(uint64_t dpy,  OverlayPortParam* const* params, unsigned int num);

    // setOverlaySessionMode() sets the overlay session mode
    status_t setOverlaySessionMode(uint64_t dpy, HWC_DISP_MODE mode);

    // getOverlaySessionMode() gets the overlay session mode
    HWC_DISP_MODE getOverlaySessionMode(uint64_t dpy);

    // getOverlaySessionInfo() gets specific display device information
    status_t getOverlaySessionInfo(uint64_t dpy, SessionInfo* info);

    // getAvailableOverlayInput gets available amount of overlay input
    // for different session
    unsigned int getAvailableOverlayInput(uint64_t dpy);

    // prepareOverlayInput() gets timeline index and fence fd of overlay input layer
    void prepareOverlayInput(uint64_t dpy, OverlayPrepareParam* param);

    // updateOverlayInputs() updates multiple overlay input layers
    void updateOverlayInputs(uint64_t dpy, OverlayPortParam* const* params, unsigned int num, sp<ColorTransform> color_transform);

    // prepareOverlayOutput() gets timeline index and fence fd for overlay output buffer
    void prepareOverlayOutput(uint64_t dpy, OverlayPrepareParam* param);

    // disableOverlayOutput() disables overlay output buffer
    void disableOverlayOutput(uint64_t dpy);

    // enableOverlayOutput() enables overlay output buffer
    void enableOverlayOutput(uint64_t dpy, OverlayPortParam* param);

    // prepareOverlayPresentFence() gets present timeline index and fence
    void prepareOverlayPresentFence(uint64_t dpy, OverlayPrepareParam* param);

    // waitVSync() is used to wait vsync signal for specific display device
    status_t waitVSync(uint64_t dpy, nsecs_t *ts);

    // setPowerMode() is used to switch power setting for display
    void setPowerMode(uint64_t dpy, int mode);

    // to query valid layers which can handled by OVL
    bool queryValidLayer(void* ptr);

    // waitAllJobDone() use to wait driver for processing all job
    status_t waitAllJobDone(const uint64_t dpy);

    // waitRefreshRequest() is used to wait for refresh request from driver
    status_t waitRefreshRequest(unsigned int* type);

    // isDisp3X4DisplayColorTransformSupported() returns whether DISP_PQ supports
    // 3X4 color matrix or not
    bool isDisp3X4DisplayColorTransformSupported();

    // isDispAodForceDisable return display forces disable aod on hwcomposer
    bool isDispAodForceDisable();

    // getMaxOverlayHeight() gets overlay supported height amount
    uint32_t getMaxOverlayHeight();

    // getMaxOverlayWidth() gets overlay supported width amount
    uint32_t getMaxOverlayWidth();

    // getSupportedColorMode is used to check what colormode device support
    int32_t getSupportedColorMode();

    // getDisplayOutputRotated() get the decouple buffer is rotated or not
    int32_t getDisplayOutputRotated();

    // getRszMaxWidthInput() get the max width of rsz input
    uint32_t getRszMaxWidthInput();

    // getRszMaxHeightInput() get the max height of rsz input
    uint32_t getRszMaxHeightInput();

    // enableDisplayFeature() is used to force hwc to enable feature
    void enableDisplayFeature(uint32_t flag);

    // disableDisplayFeature() is used to force hwc to disable feature
    void disableDisplayFeature(uint32_t flag);


    // DrmDevice interface
    // getDrmSessionMode() return the definition of drm session mode
    uint32_t getDrmSessionMode(uint64_t dpy);

    // getHrtIndex() return the display index of HRT
    // In the DRM HWC, external and virtual display are not always filled in second array.
    // their index are decided by crtc's pipe.
    uint32_t getHrtIndex(uint64_t dpy);

    // getWidth() gets the width from the config
    int32_t getWidth(uint64_t dpy, hwc2_config_t config);

    // getHeight() gets the height from the config
    int32_t getHeight(uint64_t dpy, hwc2_config_t config);

    // getRefresh() gets the fps from the config
    int32_t getRefresh(uint64_t dpy, hwc2_config_t config);

    // getNumConfigs gets the number of configs
    uint32_t getNumConfigs(uint64_t dpy);

    // dump dev info
    void dump(const uint64_t& dpy, String8* dump_str);

    // updateDisplayResolution use to update display resolution
    int32_t updateDisplayResolution(uint64_t dpy);

    // getCurrentRefresh() gets the current mode fps
    // this is only used for external display
    int32_t getCurrentRefresh(uint64_t dpy);

    // query if HWC_FEATURE support or not
    bool isHwcFeatureSupported(uint32_t hwc_feature_flag);

    // MML decouple IOCtrl, after calling this ioctrl MML start to work
    void submitMML(uint64_t dpy, struct mml_submit& params);

    // isColorHistogramtSupport() is used to query color histogram suuported or not
    int32_t isColorHistogramtSupport();

    // getHistogramAttribute() is used to get capability of color histogram
    int32_t getHistogramAttribute(int32_t* color_format, int32_t* data_space,
            uint8_t* mask, uint32_t* max_bin);

    // enableHistogram() is used to start/stop histogram
    int32_t enableHistogram(const bool enable, const int32_t format,
            const uint8_t format_mask, const int32_t dataspace, const uint32_t bin_count);

    // collectHistogram() is used to get the histogram of last frame
    int32_t collectHistogram(uint32_t* fence_index,
            uint32_t* histogram_ptr[NUM_FORMAT_COMPONENTS]);

    // get the unique id of display (the related driver use it to identify display module)
    uint32_t getDisplayUniqueId(uint64_t dpy);

    int32_t getCompressionDefine(const char* name, uint64_t* type, uint64_t* modifier);

    // Display Driver debug log IOCtrl
    void enableDisplayDriverLog(uint32_t param);

    const MSync2Data::ParamTable* getMSyncDefaultParamTable();

private:
    DrmDevice();

    struct disp_ccorr_config
    {
        int mode;
        int color_matrix[16];
        bool feature_flag;
    };

    struct FbCacheEntry
    {
        uint64_t alloc_id;
        uint32_t fb_id;
        unsigned int format;

        uint64_t used_at_count; // set to FbCacheInfo::count, every time this entry is used.
    };

    struct FbCacheInfo
    {
        FbCacheInfo(const OverlayPortParam* param);
        bool paramIsSame(const OverlayPortParam* param);
        void updateParam(const OverlayPortParam* param);

        uint64_t id;
        unsigned int src_buf_width;
        unsigned int src_buf_height;
        unsigned int pitch;
        unsigned int format;
        bool secure;

        std::list<FbCacheEntry> fb_caches;
        uint64_t count; // update every buf update, to check which cache is not used anymore
        uint64_t last_alloc_id;
        nsecs_t last_buf_update;
    };

    struct FbCache
    {
        std::list<FbCacheInfo> layer_caches;

        std::list<FbCacheEntry> fb_caches_pending_remove; // to be removed after atomic commit
        void moveFbCachesToRemove(FbCacheEntry& entry);
        void moveFbCachesToRemove(FbCacheInfo* cache);
        void moveFbCachesToRemoveExcept(FbCacheInfo* cache, uint32_t fb_id);

        FbCacheInfo* getLayerCacheForId(uint64_t id);
        void dump(String8* str);
    };

    // query hw capabilities through ioctl and store in m_caps_info
    void queryCapsInfo();

    // get the correct device id for extension display when enable dual display
    unsigned int getDeviceId(uint64_t dpy);

    status_t disablePlane(drmModeAtomicReqPtr req_ptr, const DrmModePlane* plane);
    void createAtomicRequirement(uint64_t dpy);
    void releaseAtomicRequirement(uint64_t dpy);
    status_t disableCrtcOutput(drmModeAtomicReqPtr req_ptr, const DrmModeCrtc* crtc);

    void createFbId(OverlayPortParam* param, const uint64_t& dpy, const uint64_t& id);
    status_t createColorTransformBlob(const uint64_t& dpy, sp<ColorTransform> color_transform, uint32_t* id);
    status_t destroyBlob(uint32_t id);

    void trashCleanerLoop();
    void trashAddFbId(const std::list<FbCacheEntry>& fb_caches);
    void trashAddFbId(FbCacheEntry& entry);

    void removeFbCacheDisplay(uint64_t dpy);
    void removeFbCacheAllDisplay();

    void updateMSyncEnable(uint64_t dpy, DataPackage* package, DataPackage* late_package);
    void updateMSyncParamTable(uint64_t dpy, DataPackage* package, DataPackage* late_package);
    void getMSyncDefaultParamTableInternal();

    FrameCfg m_frame_cfg[DisplayManager::MAX_DISPLAYS];

    mtk_drm_disp_caps_info m_caps_info;

    DrmModeResource* m_drm = nullptr;
    drmModeAtomicReqPtr m_atomic_req[DisplayManager::MAX_DISPLAYS];

    unsigned int m_max_overlay_num;

    // this mutex is used to protect layer_caches of m_fb_caches
    // layer_caches is accessed by OverlayEngine thread and dump thread
    // we need to guarantee that layer_caches can not be modified when dump thread access it
    mutable std::mutex m_layer_caches_mutex[DisplayManager::MAX_DISPLAYS];
    FbCache m_fb_caches[DisplayManager::MAX_DISPLAYS];

    // This is for decouple buffer record
    std::pair<uint64_t, uint32_t >* m_prev_commit_dcm_out_fb_id[DisplayManager::MAX_DISPLAYS];
    DisplayState m_display_state;

    // store the last commited blob id of color transform
    uint32_t m_prev_commit_color_transform[DisplayManager::MAX_DISPLAYS];

    // CRTC color transform result
    int m_crtc_colortransform_res[DisplayManager::MAX_DISPLAYS];
    struct disp_ccorr_config m_last_color_config[DisplayManager::MAX_DISPLAYS];

    uint32_t m_drm_max_support_width;
    uint32_t m_drm_max_support_height;

    std::thread m_trash_cleaner_thread;
    mutable std::mutex m_trash_mutex;
    mutable std::condition_variable m_condition;
    bool m_trash_cleaner_thread_stop = false;
    std::list<uint32_t> m_trash_fb_id_list;
    mutable std::mutex m_add_trash_mutex;
    bool m_trash_request_add_fb_id = false;

    bool m_msync2_enable[DisplayManager::MAX_DISPLAYS] = {0};
    // currently only for primary
    MSync2Data::ParamTable m_msync_param_table;

    DrmHistogramDevice m_drm_histogram;
};
#endif // DRM_HWDEV_H_
