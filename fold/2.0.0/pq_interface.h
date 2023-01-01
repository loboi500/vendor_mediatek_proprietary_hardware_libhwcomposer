#ifndef __HWC_PQ_DEV_H__
#define __HWC_PQ_DEV_H__

#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <cutils/native_handle.h>

#include "pq_xml_parser.h"

#ifdef USES_PQSERVICE
#include <vendor/mediatek/hardware/pq/2.14/IPictureQuality.h>
#include <vendor/mediatek/hardware/pq/2.14/types.h>
using vendor::mediatek::hardware::pq::V2_14::IPictureQuality;
using vendor::mediatek::hardware::pq::V2_14::IAiBldCallback;
using vendor::mediatek::hardware::pq::V2_14::ai_bld_config;
using ::android::hardware::Return;
using ::android::hardware::Void;
using android::hardware::hidl_vec;
using android::hardware::hidl_handle;
#endif

using namespace android;

#define DEFAULT_IDENTITY_VALUE 1024

class IPqDevice : public RefBase
{
public:
    IPqDevice();
    ~IPqDevice();

    virtual bool setColorTransform(const float* matrix, const int32_t& hint);

    virtual bool isColorTransformIoctl();
    virtual void useColorTransformIoctl(int32_t useIoctl);

    virtual void setGamePQHandle(const buffer_handle_t& handle);

    virtual int setDisplayPqMode(const uint64_t connector_id, const uint32_t crtc_id,
            const int32_t pq_mode_id, const int prev_present_fence);

    virtual int getCcorrIdentityValue();

    virtual void setAiBldBuffer(const buffer_handle_t& handle, uint32_t pf_fence_idx);

    virtual bool afterPresent();

    virtual void resetPqService();

    virtual void setMainConnector(uint32_t connector_id);
    virtual void switchConnector(uint32_t connector_id);

#ifdef USES_PQSERVICE
protected:
    class DeathRecipient : public android::hardware::hidl_death_recipient
    {
    public:
        DeathRecipient(IPqDevice* pq_device)
            : m_pq_device(pq_device)
        {}
    private:
        void serviceDied(uint64_t cookie, const android::wp<::android::hidl::base::V1_0::IBase>& who);

        IPqDevice* m_pq_device;
    };

    class AiBldCallback: public IAiBldCallback
    {
    private:
        virtual Return<void> AIBldeEnable(uint32_t featureId, uint32_t enable);
        virtual Return<void> AIBldParams(const ai_bld_config& aiBldConfig);
        virtual Return<void> perform(uint32_t op_code, const hidl_vec<uint8_t> &input_params,
                                     const hidl_vec<hidl_handle> &input_handles);
    };
#endif

protected:
    virtual void checkAndOpenIoctl() = 0;

    virtual bool setColorTransformViaIoctl(const float* matrix, const int32_t& hint) = 0;
#ifdef USES_PQSERVICE
    virtual sp<IPictureQuality> getPqServiceLocked(bool timeout_break = false, int retry_limit = 10);
    virtual bool setColorTransformViaService(const float* matrix, const int32_t& hint);
    virtual int setDisplayPqModeLocked(const uint64_t connector_id, const uint32_t crtc_id,
            const int32_t pq_mode_id, const int prev_present_fence);
    virtual void setMainConnectorLocked(uint32_t connector_id);
    virtual void switchConnectorLocked(uint32_t connector_id);
#endif

protected:
    int m_pq_fd;

    Mutex m_lock;
    bool m_use_ioctl;

#ifdef USES_PQSERVICE
    sp<IPictureQuality> m_pq_service;
    sp<DeathRecipient> m_pq_death_recipient;
#endif

    // should hold a main connector id and current connector id
    // since pq device should not get hwcmediator here
    uint32_t m_main_connector_id;
    uint32_t m_curr_connector_id;

    // used to notify hwcdisplay if pq service died between two present
    // if so, should notify it to recover previous status
    bool m_pq_service_died;
};

IPqDevice* getPqDevice();

#endif
