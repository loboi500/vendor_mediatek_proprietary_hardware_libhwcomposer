#ifndef __MTK_HWC_DRM_MODE_RESOURCE_H__
#define __MTK_HWC_DRM_MODE_RESOURCE_H__

#include <stdint.h>
#include <vector>

#include <xf86drm.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#include <xf86drmMode.h>
#pragma clang diagnostic pop

#include <linux/mediatek_drm.h>

#include <mutex>
#include <utils/Timers.h>
#include <unordered_set>

#include "dev_interface.h"
#include "drmmodeutils.h"

class DrmModeCrtc;
class DrmModeEncoder;
class DrmModeConnector;
class DrmModePlane;

struct hwc_drm_bo;

class DrmModeResource
{
public:
    static DrmModeResource& getInstance();
    ~DrmModeResource();

    int getFd();

    void dumpResourceInfo();
    int connectDisplay(uint64_t dpy);
    void connectAllDisplay();
    int setDisplay(uint64_t dpy, bool use_req_size);
    int blankDisplay(uint64_t dpy, int mode);

    int addFb(struct hwc_drm_bo *fb_bo);
    int addFb(uint32_t gem_handle, unsigned int width, unsigned int height,
              unsigned int stride, unsigned int format, int blending, bool secure,
              uint32_t *fb_id);
    int removeFb(uint32_t fb_id);
    int allocateBuffer(struct hwc_drm_bo *fb_bo);
    int freeBuffer(struct hwc_drm_bo &fb_bo);
    int getHandleFromPrimeFd(int fd, uint32_t* gem_handle);

    DrmModeCrtc* getDisplay(uint64_t dpy);
    int waitNextVsync(uint64_t dpy, nsecs_t* ts);

    int atomicCommit(drmModeAtomicReqPtr req, uint32_t flags, void *user_data);
    inline int ioctl(unsigned long request, void *arg) { return ::ioctl(m_fd, static_cast<unsigned int>(request), arg); }
    inline int drmIoctl(unsigned long request, void *arg) { return ::drmIoctl(m_fd, request, arg); }

    uint32_t getDimFbId() { return m_dim_fb_id; }

    int32_t getWidth(uint64_t dpy, uint32_t config);
    int32_t getHeight(uint64_t dpy, uint32_t config);
    int32_t getRefresh(uint64_t dpy, uint32_t config);
    uint32_t getNumConfigs(uint64_t dpy);

    int createPropertyBlob(const void *data, size_t length, uint32_t *id);
    int destroyPropertyBlob(uint32_t id);

    uint32_t getMaxSupportWidth();
    uint32_t getMaxSupportHeight();

    int32_t updateCrtcToPreferredModeInfo(uint64_t dpy);

    // getCurrentRefresh() gets the current mode fps
    // this is only used for external display
    int32_t getCurrentRefresh(uint64_t dpy);

    const mtk_drm_disp_caps_info& getCapsInfo();

    void dump(String8* str);
private:
    DrmModeResource();
    int init();
    void queryCapsInfo();

    int initDrmCap();
    int initDrmResource();
    int initDrmCrtc(drmModeResPtr r);
    int initDrmEncoder(drmModeResPtr r);
    int initDrmConnector(drmModeResPtr r);
    int initDrmPlane();
    void arrangeResource();
    void initDimFbId();
    void setupDisplayList();
    DrmModeConnector* getCurrentConnector(uint64_t dpy);

private:
    int m_fd;

    std::vector<DrmModeCrtc*> m_crtc_list;
    std::vector<DrmModeEncoder*> m_encoder_list;
    std::vector<DrmModeConnector*> m_connector_list;
    std::vector<DrmModePlane*> m_plane_list;
    DrmModeCrtc* m_display_list[DisplayManager::MAX_DISPLAYS];

    uint32_t m_dim_fb_id;
    uint32_t m_max_support_width;
    uint32_t m_max_support_height;

    std::unordered_set<uint32_t> m_cur_fb_ids;
    std::mutex m_cur_fb_lock;

    mtk_drm_disp_caps_info m_caps_info;
};

#endif
