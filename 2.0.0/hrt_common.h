#ifndef HWC_HRT_INTERFACE_H
#define HWC_HRT_INTERFACE_H

#include <sstream>
#include <vector>

#include <utils/StrongPointer.h>
#include <utils/String8.h>
#include <utils/RefBase.h>

#include "display.h"

using namespace android;

enum
{
    HWC_DISP_OVERRIDE_MDP_OUTPUT_FORMAT_DEFAULT = 0,
};

class HWCDisplay;
struct HrtLayerConfig;

class HrtCommon : public RefBase
{
public:
    HrtCommon()
    {
        memset(m_layer_config_len, 0, sizeof(m_layer_config_len));
        memset(m_hrt_config_list, 0, sizeof(m_hrt_config_list));
        memset(m_hrt_config_len, 0, sizeof(m_hrt_config_len));
    }

    virtual ~HrtCommon() {}

    virtual bool isEnabled() const;

    virtual bool isRPOEnabled() const;

    virtual void dump(String8* str);

    virtual void printQueryValidLayerResult();

    virtual void modifyMdpDstRoiIfRejectedByRpo(const std::vector<sp<HWCDisplay> >& displays);

    virtual void run(std::vector<sp<HWCDisplay> >& displays, const bool& is_skip_validate);

    void simpleLayeringRule(const std::vector<sp<HWCDisplay> >& displays);

    virtual void fillLayerConfigList(const std::vector<sp<HWCDisplay> >& /*displays*/) {}

    virtual void fillDispLayer(const std::vector<sp<HWCDisplay> >& /*displays*/) {}

    virtual void fillLayerInfoOfDispatcherJob(const std::vector<sp<HWCDisplay> >& displays);

    virtual void setCompType(const std::vector<sp<HWCDisplay> >& displays);

    virtual bool queryValidLayer() { return false;}

    // this function only called before hrt caculate for updating hwc_gles range
    void updateGlesRangeForDisplaysBeforHrt(const std::vector<sp<HWCDisplay> >& displays);

    // this function only called after hrt for updating hwtype in gles_range
    void updateGlesRangeHwTypeForDisplays(const std::vector<sp<HWCDisplay> >& displays);
protected:
    std::stringstream m_hrt_result;
    unsigned int m_layer_config_len[DisplayManager::MAX_DISPLAYS];
    HrtLayerConfig* m_hrt_config_list[DisplayManager::MAX_DISPLAYS];
    unsigned int m_hrt_config_len[DisplayManager::MAX_DISPLAYS];
};

HrtCommon* createHrt();

#endif
