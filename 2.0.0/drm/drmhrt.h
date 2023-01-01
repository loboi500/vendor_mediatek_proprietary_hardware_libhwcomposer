#ifndef HWC_DRM_HRT_H
#define HWC_DRM_HRT_H

#include <stdint.h>
#include <linux/mediatek_drm.h>

#include "hrt_common.h"
#include "display.h"
#include "mtk-mml.h"

struct HrtLayerConfig;

class DrmHrt : public HrtCommon
{
public:
    DrmHrt()
    {
        HrtCommon();
        memset(m_layer_config_list, 0, sizeof(m_layer_config_list));
        memset(&m_disp_layer, 0, sizeof(m_disp_layer));
        memset(m_layer_mml_info, 0, sizeof(m_layer_mml_info));
    }
    ~DrmHrt()
    {
        for (int i = 0; i < DisplayManager::MAX_DISPLAYS; ++i)
        {
            if (m_layer_config_list[i])
                free(m_layer_config_list[i]);
        }
    }
    bool isEnabled() const;

    bool isRPOEnabled() const;

    void printQueryValidLayerResult();

    void fillLayerConfigList(const std::vector<sp<HWCDisplay> >& displays);

    void fillDispLayer(const std::vector<sp<HWCDisplay> >& displays);

    void fillLayerInfoOfDispatcherJob(const std::vector<sp<HWCDisplay> >& displays);

    bool queryValidLayer();

private:
    mml_frame_info* m_layer_mml_info[DisplayManager::MAX_DISPLAYS];
    drm_mtk_layer_config* m_layer_config_list[DisplayManager::MAX_DISPLAYS];
    drm_mtk_layering_info m_disp_layer;
};

#endif
