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
#include <pq_perform.h>
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
    , m_main_connector_id(UINT32_MAX)
    , m_curr_connector_id(UINT32_MAX)
    , m_pq_service_died(false)
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

int IPqDevice::setDisplayPqMode(const uint64_t connector_id, const uint32_t crtc_id,
        const int32_t pq_mode_id, const int prev_present_fence)
{
    int pq_fence_fd = -1;
#ifdef USES_PQSERVICE
    {
        Mutex::Autolock lock(m_lock);
        sp<IPictureQuality> pq_service = getPqServiceLocked();
        if (pq_service == nullptr)
        {
            HWC_LOGE("%s: cannot find PQ service!", __func__);
        }
        else
        {
            pq_fence_fd = setDisplayPqModeLocked(connector_id, crtc_id,
                    pq_mode_id, prev_present_fence);
        }
    }

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

        if (m_main_connector_id != UINT32_MAX)
        {
            HWC_LOGI("%s: pq_service died before, set connector id to it [%d, %d]", __func__,
                        m_main_connector_id, m_curr_connector_id);
            setMainConnectorLocked(m_main_connector_id);
            if (m_curr_connector_id == UINT32_MAX)
            {
                switchConnectorLocked(m_main_connector_id);
            }
        }
        if (m_curr_connector_id != UINT32_MAX)
        {
            switchConnectorLocked(m_curr_connector_id);
        }

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

int IPqDevice::setDisplayPqModeLocked(const uint64_t connector_id, const uint32_t crtc_id,
        const int32_t pq_mode_id, const int prev_present_fence)
{
    int pq_fence_fd = -1;

    if (m_pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        if (crtc_id == 0)
        {
            HWC_LOGW("%s: set pq mode with a dubious crtc id[%u]", __func__, crtc_id);
        }
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
        m_pq_service->setColorModeWithFence(static_cast<uint32_t>(pmi),
                fence_handle, static_cast<uint32_t>(connector_id), crtc_id,
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

bool IPqDevice::afterPresent()
{
#ifdef USES_PQSERVICE
    if (HwcFeatureList::getInstance().getFeature().is_support_pq > 0)
    {
        Mutex::Autolock lock(m_lock);
        if (m_pq_service_died)
        {
            getPqServiceLocked(true, 0);
            m_pq_service_died = false;
            return false;
        }
    }
#endif
    return true;
}

void IPqDevice::resetPqService()
{
#ifdef USES_PQSERVICE
    AiBluLightDefender::getInstance().setEnable(false);
    Mutex::Autolock lock(m_lock);
    m_pq_service = nullptr;
    m_pq_service_died = true;
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

void IPqDevice::setMainConnector(uint32_t connector_id)
{
#ifdef USES_PQSERVICE
    Mutex::Autolock lock(m_lock);
    HWC_LOGD("%s(), connector_id %d", __func__, connector_id);

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        setMainConnectorLocked(connector_id);
    }

#else
    (void) connector_id
#endif

    m_main_connector_id = connector_id;
    if (m_curr_connector_id == UINT32_MAX)
    {
        m_curr_connector_id = connector_id;
    }
}

void IPqDevice::switchConnector(uint32_t connector_id)
{
#ifdef USES_PQSERVICE
    Mutex::Autolock lock(m_lock);
    HWC_LOGD("%s(), connector_id %d", __func__, connector_id);

    if (m_curr_connector_id == connector_id)
    {
        return;
    }

    sp<IPictureQuality> pq_service = getPqServiceLocked();
    if (pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        switchConnectorLocked(connector_id);
    }

#else
    (void) connector_id
#endif

    m_curr_connector_id = connector_id;
}

#ifdef USES_PQSERVICE
// should not use getPqServiceLocked() inside, may cause deadlock
// and should not use lock, too, since this might call by getPqServiceLocked()
void IPqDevice::setMainConnectorLocked(uint32_t connector_id)
{
    HWC_LOGI("%s(), connector_id %d", __func__, connector_id);

    if (m_pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        ATRACE_NAME("call pq perform");
        int error = 0;
        android::hardware::hidl_vec<uint8_t> param;
        param.setToExternal(reinterpret_cast<uint8_t*>(&connector_id), sizeof(connector_id));
        android::hardware::hidl_vec<hidl_handle> handle;
        auto hidl_cb = [&error/*, &ret_params*/] (int32_t err, hidl_vec<uint8_t> /*params*/, hidl_vec<hidl_handle> /*handles*/) {
            error = err;
        };
        auto status = m_pq_service->perform(0, DisplayPQExt::OpCode::mSETMAINCONNECTOR, param, handle, hidl_cb);
        if (!status.isOk() || error != 0)
        {
            HWC_LOGW("%s: status: %s, error: %d", __func__, status.description().c_str(), error);
        }
    }
}

void IPqDevice::switchConnectorLocked(uint32_t connector_id)
{
    HWC_LOGI("%s(), connector_id %d", __func__, connector_id);

    if (m_pq_service == nullptr)
    {
        HWC_LOGE("%s: cannot find PQ service!", __func__);
    }
    else
    {
        ATRACE_NAME("call pq perform");
        int error = 0;
        android::hardware::hidl_vec<uint8_t> param;
        param.setToExternal(reinterpret_cast<uint8_t*>(&connector_id), sizeof(connector_id));
        android::hardware::hidl_vec<hidl_handle> handle;
        auto hidl_cb = [&error/*, &ret_params*/] (int32_t err, hidl_vec<uint8_t> /*params*/, hidl_vec<hidl_handle> /*handles*/) {
            error = err;
        };
        auto status = m_pq_service->perform(0, DisplayPQExt::OpCode::mCONNECTORSWITCH, param, handle, hidl_cb);
        if (!status.isOk() || error != 0)
        {
            HWC_LOGW("%s: status: %s, error: %d", __func__, status.description().c_str(), error);
        }
    }
}
#endif
