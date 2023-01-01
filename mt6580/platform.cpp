#define DEBUG_LOG_TAG "PLAT"

#include <hardware/hwcomposer.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "utils/debug.h"
#include "utils/tools.h"
#include "utils/perfhelper.h"

#include "hwc2.h"
#include "platform.h"

#include "DpBlitStream.h"

#include "mtkperf_resource.h"

extern unsigned int mapDpOrientation(const uint32_t transform);

// ---------------------------------------------------------------------------
Platform_MT6580::Platform_MT6580()
{
    m_config.platform = PLATFORM_MT6580;

    m_config.compose_level = COMPOSE_ENABLE_ALL;

    m_config.mirror_state = MIRROR_ENABLED;

    m_config.use_async_bliter_ultra = true;

    m_config.blitdev_for_virtual = true;

    m_config.is_support_ext_path_for_virtual =
        m_config.blitdev_for_virtual ? true : false;

    m_config.av_grouping = false;

    m_config.support_2subsample_with_odd_size_roi = false;

    m_config.is_ovl_support_odd_size = false;

    m_config.wdt_trace = true;
}

size_t Platform_MT6580::getLimitedExternalDisplaySize()
{
    // 4k resolution
    return 1920 * 1080;
}

bool Platform_MT6580::isUILayerValid(const sp<HWCLayer>& layer, int32_t* line)
{
    return PlatformCommon::isUILayerValid(layer, line);
}


bool Platform_MT6580::isMMLayerValid(const sp<HWCLayer>& layer, const int32_t pq_mode_id,
        int32_t* line)
{
    const int srcWidth = getSrcWidth(layer);
    const int srcHeight = getSrcHeight(layer);
    if (srcWidth == 128 && srcHeight == 128)
    {
        if (!layer->getPerfState())
        {
            layer->setPerfState(true);
            int perf_lock_resource[] = {PERF_RES_CPUFREQ_PERF_MODE, 1};
            int resource_size = sizeof(perf_lock_resource) / sizeof(*perf_lock_resource);
            PerfHelper::getInstance()->perfLockAcq(0, 15000, perf_lock_resource, resource_size);
        }
        *line = __LINE__ + 10000;
        return false;
    }
    return PlatformCommon::isMMLayerValid(layer, pq_mode_id, line);
}
