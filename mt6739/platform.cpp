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
extern unsigned int mapDpOrientation(const uint32_t transform);

// ---------------------------------------------------------------------------
Platform_MT6739::Platform_MT6739()
{
    m_config.platform = PLATFORM_MT6739;

    m_config.compose_level = COMPOSE_ENABLE_ALL;

    m_config.mirror_state = MIRROR_ENABLED;

    m_config.rdma_roi_update = 1;

    m_config.use_async_bliter_ultra = true;

    m_config.enable_smart_layer = true;

    m_config.enable_rgba_rotate = true;

    m_config.blitdev_for_virtual = true;

    m_config.support_color_transform = true;

    m_config.is_support_ext_path_for_virtual =
        m_config.blitdev_for_virtual ? true : false;

    m_config.av_grouping = false;

    m_config.is_client_clear_support = true;
}

size_t Platform_MT6739::getLimitedExternalDisplaySize()
{
    // 4k resolution
    return 3840 * 2160;
}


bool Platform_MT6739::isUILayerValid(const sp<HWCLayer>& layer, int32_t* line)
{
    return PlatformCommon::isUILayerValid(layer, line);
}


bool Platform_MT6739::isMMLayerValid(const sp<HWCLayer>& layer, const int32_t pq_mode_id,
        int32_t* line)
{
    const int src_width = getSrcWidth(layer);
    const int dst_width = WIDTH(layer->getDisplayFrame());
    const int src_left = getSrcLeft(layer);
    const int src_top = getSrcTop(layer);

    const PrivateHandle& priv_hnd = layer->getPrivateHandle();

    // Workaround
    if (src_left == 0 && src_top == 0 && src_width == 128 && dst_width == 720
        && (priv_hnd.format == HAL_PIXEL_FORMAT_RGB_565 || \
            priv_hnd.format == HAL_PIXEL_FORMAT_RGB_888 || \
            priv_hnd.format == HAL_PIXEL_FORMAT_RGBX_8888))
    {
        *line = __LINE__ + 20000;
        return false;
    }

    return PlatformCommon::isMMLayerValid(layer, pq_mode_id, line);
}
