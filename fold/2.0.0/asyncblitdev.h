#ifndef __HWC_ASYNC_BLITDEV_H__
#define __HWC_ASYNC_BLITDEV_H__

#include "dev_interface.h"

#include <unordered_map>
#include <utils/Vector.h>
#include "DpAsyncBlitStream2.h"

using namespace android;

struct AsyncBlitInvalidParams
{
    AsyncBlitInvalidParams()
        : src_ion_fd(-1)
        , dst_ion_fd(-1)
        , src_fence_index(0)
        , dst_fence_index(0)
        , present_fence_index(0)
        , src_fmt(0)
        , dst_fmt(0)
        , src_range(0)
        , dst_range(0)
        , src_is_need_flush(false)
        , dst_is_need_flush(false)
        , src_is_secure(false)
        , dst_is_secure(false)
        , job_sequence(0)
    {
        memset(&src_crop, 0, sizeof(src_crop));
        memset(&dst_crop, 0, sizeof(dst_crop));
    }

    int             src_ion_fd;
    int             dst_ion_fd;
    unsigned int    src_fence_index;
    unsigned int    dst_fence_index;
    unsigned int    present_fence_index;
    unsigned int    src_fmt;
    unsigned int    dst_fmt;
    Rect            src_crop;
    Rect            dst_crop;
    unsigned int    src_range;
    unsigned int    dst_range;
    bool            src_is_need_flush;
    bool            dst_is_need_flush;
    bool            src_is_secure;
    bool            dst_is_secure;
    uint64_t        job_sequence;
};

struct MdpJobInfo
{
    MdpJobInfo()
        : id(0)
        , release_fd(-1)
        , input_ion_fd(-1)
        , output_ion_fd(-1)
    {
    }

    uint32_t id;
    int release_fd;
    int input_ion_fd;
    int output_ion_fd;
};

class AsyncBlitDevice : public IOverlayDevice
{
public:
    AsyncBlitDevice();
    ~AsyncBlitDevice();

    int32_t getType() { return OVL_DEVICE_TYPE_BLITDEV; }

    // initOverlay() initializes overlay related hw setting
    void initOverlay();

    // getHwVersion() is used to get the HW version for check platform family
    static unsigned int getHwVersion();

    // isDispRszSupported() is used to query if display rsz is supported
    bool isDispRszSupported();

    // isDispRszSupported() is used to query if display rsz is supported
    bool isDispRpoSupported();

    // isDisp3X4DisplayColorTransformSupported() returns whether DISP_PQ supports
    // 3X4 color matrix or not
    bool isDisp3X4DisplayColorTransformSupported();

    // isDispAodForceDisable return display forces disable aod on hwcomposer.
    bool isDispAodForceDisable();

    // isPartialUpdateSupported() is used to query if PartialUpdate is supported
    bool isPartialUpdateSupported();

    // isFenceWaitSupported() is used to query if FenceWait is supported
    bool isFenceWaitSupported();

    // isConstantAlphaForRGBASupported() is used to query if PRGBA is supported
    bool isConstantAlphaForRGBASupported();

    // isDispSelfRefreshSupported is used to query if hardware support ioctl of self-refresh
    bool isDispSelfRefreshSupported();

    // isDisplayHrtSupport() is used to query HRT suuported or not
    bool isDisplayHrtSupport();

    // isDisplaySupportedWidthAndHeight() is used to query Width and Height is supported or not
    bool isDisplaySupportedWidthAndHeight(unsigned int width, unsigned int height);

    // getSupportedColorMode is used to check what colormode device support
    int32_t getSupportedColorMode();

    // getMaxOverlayInputNum() gets overlay supported input amount
    unsigned int getMaxOverlayInputNum();

    // getMaxOverlayHeight() gets overlay supported height amount
    uint32_t getMaxOverlayHeight();

    // getMaxOverlayWidth() gets overlay supported width amount
    uint32_t getMaxOverlayWidth();

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

    // createOverlaySession() creates overlay composition session
    status_t createOverlaySession(
        uint64_t dpy, uint32_t drm_id_crtc, uint32_t width, uint32_t height,
        HWC_DISP_MODE mode = HWC_DISP_SESSION_DIRECT_LINK_MODE);

    // destroyOverlaySession() destroys overlay composition session
    void destroyOverlaySession(uint64_t dpy, uint32_t drm_id_crtc);

    // truggerOverlaySession() used to trigger overlay engine to do composition
    status_t triggerOverlaySession(
        uint64_t dpy, uint32_t drm_id_crtc, int present_fence_idx, int ovlp_layer_num,
        int prev_present_fence_fd, hwc2_config_t config,
        const uint32_t& hrt_weight, const uint32_t& hrt_idx,
        unsigned int num, OverlayPortParam* const* params,
        sp<ColorTransform> color_transform,
        TriggerOverlayParam trigger_param
        );

    // disableOverlaySession() usd to disable overlay session to do composition
    void disableOverlaySession(uint64_t dpy, uint32_t drm_id_crtc, OverlayPortParam* const* params, unsigned int num);

    // setOverlaySessionMode() sets the overlay session mode
    status_t setOverlaySessionMode(uint64_t dpy, HWC_DISP_MODE mode);

    // getOverlaySessionMode() gets the overlay session mode
    HWC_DISP_MODE getOverlaySessionMode(uint64_t dpy, uint32_t drm_id_crtc);

    // getOverlaySessionInfo() gets specific display device information
    status_t getOverlaySessionInfo(uint64_t dpy, uint32_t drm_id_crtc, SessionInfo* info);

    // getAvailableOverlayInput gets available amount of overlay input
    // for different session
    unsigned int getAvailableOverlayInput(uint64_t dpy, uint32_t drm_id_crtc);

    // prepareOverlayInput() gets timeline index and fence fd of overlay input layer
    void prepareOverlayInput(uint64_t dpy, OverlayPrepareParam* param);

    // updateOverlayInputs() updates multiple overlay input layers
    void updateOverlayInputs(uint64_t dpy, uint32_t drm_id_crtc,
                             OverlayPortParam* const* params,
                             unsigned int num,
                             sp<ColorTransform> color_transform);

    // prepareOverlayOutput() gets timeline index and fence fd for overlay output buffer
    void prepareOverlayOutput(uint64_t dpy, OverlayPrepareParam* param);

    // disableOverlayOutput() disables overlay output buffer
    void disableOverlayOutput(uint64_t dpy, uint32_t drm_id_crtc);

    // enableOverlayInput() enables single overlay input layer
    void enableOverlayInput(uint64_t dpy, OverlayPortParam* param, int id);

    // prepareOverlayPresentFence() gets present timeline index and fence
    void prepareOverlayPresentFence(uint64_t dpy, OverlayPrepareParam* param);

    // waitVSync() is used to wait vsync signal for specific display device
    status_t waitVSync(uint64_t dpy, uint32_t drm_id_crtc, nsecs_t *ts);

    // setPowerMode() is used to switch power setting for display
    void setPowerMode(uint64_t dpy, uint32_t drm_id_crtc, int mode, bool panel_stay_on = false);

    // enableOverlayOutput() enables overlay output buffer
    void enableOverlayOutput(uint64_t dpy, uint32_t drm_id_crtc, OverlayPortParam* param);

    // to query valid layers which can handled by OVL
    bool queryValidLayer(void* disp_layer);

    // waitAllJobDone() use to wait driver for processing all job
    status_t waitAllJobDone(const uint64_t dpy);

    // waitRefreshRequest() is used to wait for refresh request from driver
    status_t waitRefreshRequest(unsigned int* type);

    // getWidth() gets the width from the config
    int32_t getWidth(uint64_t dpy, uint32_t drm_id_connector, hwc2_config_t config);

    // getHeight() gets the height from the config
    int32_t getHeight(uint64_t dpy, uint32_t drm_id_connector, hwc2_config_t config);

    // getRefresh() gets the fps from the config
    int32_t getRefresh(uint64_t dpy, uint32_t drm_id_connector, hwc2_config_t config);

    // getNumConfigs gets the number of configs
    uint32_t getNumConfigs(uint64_t dpy, uint32_t drm_id_connector);

    // dump dev info
    void dump(const uint64_t& dpy, String8* dump_str);

    // updateDisplayResolution use to update display resolution
    int32_t updateDisplayResolution(uint64_t dpy);

    // getCurrentRefresh() gets the current mode fps
    // this is only used for external display
    int32_t getCurrentRefresh(uint64_t dpy, uint32_t drm_id_crtc);

    // MML decouple IOCtrl, after calling this ioctrl MML start to work
    void submitMML(uint64_t dpy, struct mml_submit& params);

    // Display Driver debug log IOCtrl
    void enableDisplayDriverLog(uint32_t param);
private:

    // m_blit_stream is a bit blit stream
    DpAsyncBlitStream2 m_blit_stream;

    int m_session_id;
    SessionInfo m_disp_session_info;

    std::unordered_map<int, bool> m_ion_flush_vector;
    Vector<MdpJobInfo> m_job_list;
    mutable Mutex m_vector_lock;

    AsyncBlitInvalidParams m_cur_params;

    int m_state;

    HWC_DISP_MODE m_disp_session_state;
    mutable Mutex m_state_lock;
};

#endif
