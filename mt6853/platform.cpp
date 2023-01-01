#define DEBUG_LOG_TAG "PLAT"

#include <hardware/hwcomposer.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "utils/debug.h"
#include "utils/tools.h"

#include "platform.h"
#include "DpBlitStream.h"

#include "hwc2.h"
#include <cutils/properties.h>
extern unsigned int mapDpOrientation(const uint32_t transform);

// ---------------------------------------------------------------------------
Platform_MT6853::Platform_MT6853()
{
    char TARGET_BOARD_PLATFORM[PROPERTY_VALUE_MAX];
    property_get("ro.board.platform", TARGET_BOARD_PLATFORM, "UNKNOWN");

    m_config.platform = PLATFORM_MT6853;

    m_config.compose_level = COMPOSE_ENABLE_ALL;

    m_config.mirror_state = MIRROR_DISABLED;

    m_config.rdma_roi_update = 1;

    m_config.use_async_bliter_ultra = true;

    m_config.enable_smart_layer = true;

    m_config.enable_rgba_rotate = true;
    m_config.enable_rgbx_scaling = true;

    m_config.support_color_transform = true;

    m_config.mdp_scale_percentage = 0.1f;

    m_config.extend_mdp_capacity = false;

    m_config.av_grouping = false;

    m_config.disp_support_decompress = true;
    m_config.mdp_support_decompress = true;

    m_config.is_support_mdp_pmqos = true;

    m_config.is_ovl_support_RGBA1010102 = true;

    m_config.is_client_clear_support = true;

    if (strstr("mt6877", TARGET_BOARD_PLATFORM))
        m_config.blitdev_for_virtual = false;
    else
    {
        m_config.blitdev_for_virtual = true;
        m_config.is_support_ext_path_for_virtual =
              m_config.blitdev_for_virtual ? true : false;
    }

    m_config.plat_switch |= HWC_PLAT_SWITCH_NOTIFY_HWBINDER_TID;
}

size_t Platform_MT6853::getLimitedExternalDisplaySize()
{
    // 4k resolution
    return 3840 * 2160;
}

bool Platform_MT6853::isUILayerValid(const sp<HWCLayer>& layer, int32_t* line)
{
    return PlatformCommon::isUILayerValid(layer, line);
}


bool Platform_MT6853::isMMLayerValid(const sp<HWCLayer>& layer, const int32_t pq_mode_id,
        int32_t* line)
{
    return PlatformCommon::isMMLayerValid(layer, pq_mode_id, line);
}
