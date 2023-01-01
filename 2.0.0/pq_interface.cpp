#define DEBUG_LOG_TAG "IPqDevice"

#include "pq_interface.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/ioctl.h>

#include <ddp_pq.h>
#include <ddp_drv.h>

#include "ai_blulight_defender.h"
#include "utils/tools.h"
#include "utils/debug.h"

#ifndef MTK_HWC_USE_DRM_DEVICE
#include "legacy/pqdev_legacy.h"
#else
#include "drm/drmpq.h"
#endif

#ifdef USES_PQSERVICE
#include <vendor/mediatek/hardware/pq/2.14/IPictureQuality.h>
using android::hardware::hidl_array;
using vendor::mediatek::hardware::pq::V2_14::IPictureQuality;
using vendor::mediatek::hardware::pq::V2_0::Result;
#endif

IPqDevice::IPqDevice()
    : m_pq_fd(-1)
    , m_use_ioctl(false)
#ifdef USES_PQSERVICE
    , m_pq_death_recipient(new DeathRecipient(this))
#endif
{
#ifdef USES_PQSERVICE
    Mutex::Autolock lock(m_lock);
    m_pq_service = getPqServiceLocked(true, 0);
#endif
}

IPqDevice::~IPqDevice()
{
    if (m_pq_fd != -1)
    {
        protectedClose(m_pq_fd);
    }
}

bool IPqDevice::setColorTransform(const float* matrix, const int32_t& hint)
{
    if (isColorTransformIoctl())
    {
        return setColorTransformViaIoctl(matrix, hint);
    }
    else
    {
#ifdef USES_PQSERVICE
        return setColorTransformViaService(matrix, hint);
#else
        return false;
#endif
    }
}

bool IPqDevice::isColorTransformIoctl()
{
    Mutex::Autolock lock(m_lock);
    return m_use_ioctl;
}

void IPqDevice::useColorTransformIoctl(int32_t useIoctl)
{
    Mutex::Autolock lock(m_lock);
    m_use_ioctl = useIoctl;
}

void IPqDevice::setGamePQHandle(const buffer_handle_t& handle)
{
#ifdef USES_PQSERVICE
    Mutex::Autolock lock(m_lock);

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
        return;
    }
    else
    {
        ATRACE_NAME("call gamePQHandle impl");
        pq_service->setGamePQHandle(handle);
    }
#else
    (void) handle;
#endif
}

int IPqDevice::setDisplayPqMode(const uint64_t disp_id, const uint32_t disp_unique_id,
        const int32_t pq_mode_id, const int prev_present_fence)
{
    int pq_fence_fd = -1;
#ifdef USES_PQSERVICE
    pq_fence_fd = setDisplayPqModeViaService(disp_id, disp_unique_id, pq_mode_id,
            prev_present_fence);
#else
    (void) disp_id;
    (void) disp_unique_id;
    (void) pq_mode_id;
    (void) prev_present_fence;
#endif
    return pq_fence_fd;
}

#ifdef USES_PQSERVICE
sp<IPictureQuality> IPqDevice::getPqServiceLocked(bool timeout_break, int retry_limit)
{
    if (HwcFeatureList::getInstance().getFeature().is_support_pq <= 0)
    {
        return nullptr;
    }

    if (m_pq_service)
    {
        return m_pq_service;
    }

    int retryCount = 0;
    m_pq_service = IPictureQuality::tryGetService();

    while (m_pq_service == nullptr)
    {
        if (retryCount >= retry_limit)
        {
            HWC_LOGE("Can't get PQ service tried (%d) times", retryCount);
            if (timeout_break)
            {
                break;
            }
        }
        usleep(100000); //sleep 100 ms to wait for next get service
        m_pq_service = IPictureQuality::tryGetService();
        retryCount++;
    }

    if (m_pq_service)
    {
        android::sp<AiBldCallback> aibld_cb(new AiBldCallback());
        m_pq_service->registerAIBldCb(aibld_cb);

        m_pq_service->linkToDeath(m_pq_death_recipient, 0);
    }

    return m_pq_service;
}

bool IPqDevice::setColorTransformViaService(const float* matrix, const int32_t& hint)
{
    Mutex::Autolock lock(m_lock);

    bool res = false;

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
        res = false;
    }
    else
    {
        const unsigned int dimension = 4;
        hidl_array<float, 4, 4> send_matrix;
        for (unsigned int i = 0; i < dimension; ++i)
        {
            DbgLogger logger(DbgLogger::TYPE_HWC_LOG, 'D', "matrix ");
            for (unsigned int j = 0; j < dimension; ++j)
            {
                send_matrix[i][j] = matrix[i * dimension + j];
                logger.printf("%f,", send_matrix[i][j]);
            }
        }
        res = (pq_service->setColorTransform(send_matrix, hint, 1) == Result::OK);
    }

    return res;
}

int IPqDevice::setDisplayPqModeViaService(const uint64_t disp_id, const uint32_t disp_unique_id,
        const int32_t pq_mode_id, const int prev_present_fence)
{
    Mutex::Autolock lock(m_lock);

    int pq_fence_fd = -1;

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        if (disp_unique_id == 0)
        {
            HWC_LOGW("%s: set pq mode with a dubious unique id[%u]", __func__, disp_unique_id);
        }
        uint64_t di = disp_id;
        int32_t pmi = pq_mode_id;
        native_handle_t* fence_handle = nullptr;
        if (prev_present_fence >= 0)
        {
            fence_handle = native_handle_create(1, 0);
            if (fence_handle != nullptr)
            {
                fence_handle->data[0] = prev_present_fence;
            }
        }
        else
        {
            fence_handle = native_handle_create(0, 0);
        }
        pq_service->setColorModeWithFence(static_cast<uint32_t>(pmi),
                fence_handle, static_cast<uint32_t>(di), disp_unique_id,
                [&pq_fence_fd] (Result res, android::hardware::hidl_handle pq_fence_handle){
                    if (res == Result::OK)
                    {
                        const native_handle_t* handle = pq_fence_handle.getNativeHandle();
                        if (handle != nullptr && handle->numFds > 0)
                        {
                            pq_fence_fd = dup(handle->data[0]);
                        }
                        else
                        {
                            pq_fence_fd = -1;
                        }
                    }
                    else
                    {
                        pq_fence_fd = -1;
                    }
                });
        native_handle_delete(fence_handle);
    }

    return pq_fence_fd;
}
#endif

bool IPqDevice::supportPqXml()
{
    return m_pq_xml_parser.hasXml();;
}

const std::vector<PqModeInfo>& IPqDevice::getRenderIntent(int32_t color_mode)
{
    return m_pq_xml_parser.getRenderIntent(color_mode);
}

int IPqDevice::getCcorrIdentityValue()
{
    return DEFAULT_IDENTITY_VALUE;
}

void IPqDevice::setAiBldBuffer(const buffer_handle_t& handle, uint32_t pf_fence_idx)
{
#ifdef USES_PQSERVICE
    Mutex::Autolock lock(m_lock);

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        ATRACE_NAME("call pq setAIBldBuffer");
        Result res = pq_service->setAIBldBuffer(handle, pf_fence_idx);
        if (res != Result::OK)
        {
            HWC_LOGW("%s: fail, res %d", __func__, res);
        }
    }
#else
    (void) handle;
    (void) pf_fence_idx;
#endif
}

void IPqDevice::afterPresent()
{
#ifdef USES_PQSERVICE
    if (HwcFeatureList::getInstance().getFeature().is_support_pq > 0)
    {
        Mutex::Autolock lock(m_lock);
        if (!m_pq_service)
        {
            getPqServiceLocked(true, 0);
        }
    }
#endif
}

void IPqDevice::resetPqService()
{
#ifdef USES_PQSERVICE
    AiBluLightDefender::getInstance().setEnable(false);
    Mutex::Autolock lock(m_lock);
    m_pq_service = nullptr;
#endif
}

IPqDevice* getPqDevice()
{
#ifndef MTK_HWC_USE_DRM_DEVICE
    return &PqDeviceLegacy::getInstance();
#else
    return &PqDeviceDrm::getInstance();
#endif
}

#ifdef USES_PQSERVICE
void IPqDevice::DeathRecipient::serviceDied(uint64_t /*cookie*/, const android::wp<::android::hidl::base::V1_0::IBase>& /*who*/)
{
    HWC_LOGI("PQ service died");
    if (m_pq_device)
    {
        m_pq_device->resetPqService();
    }
}
#endif

#ifdef USES_PQSERVICE
Return<void> IPqDevice::AiBldCallback::AIBldeEnable(uint32_t /*featureId*/, uint32_t /*enable*/)
{
    // not used
    return Void();
}

Return<void> IPqDevice::AiBldCallback::AIBldParams(const ai_bld_config& aiBldConfig)
{
    HWC_LOGI("%s(), enable %d, fps %d", __FUNCTION__, aiBldConfig.enable, aiBldConfig.fps);

    AiBluLightDefender::getInstance().setEnable(aiBldConfig.enable,
                                                aiBldConfig.fps,
                                                aiBldConfig.width,
                                                aiBldConfig.height,
                                                aiBldConfig.format);
    return Void();
}

Return<void> IPqDevice::AiBldCallback::perform(uint32_t /*op_code*/, const hidl_vec<uint8_t> &/*input_params*/,
                                               const hidl_vec<hidl_handle> &/*input_handles*/)
{
    return Void();
}
#endif
