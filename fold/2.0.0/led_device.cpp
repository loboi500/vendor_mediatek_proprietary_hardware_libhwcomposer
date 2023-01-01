#define DEBUG_LOG_TAG "LEDDEV"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "led_device.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "utils/tools.h"

#define LED_DIR_NAME "lcd-backlight"
#define LED_PATH_PREFIX "/sys/class/leds"
#define LED_NODE_BRIGHTNESS "brightness"
#define LED_NODE_MAX_BRIGHTNESS "max_brightness"
#define LED_NODE_MIN_BRIGHTNESS "min_brightness"
#define LED_NODE_CONNECTOR_ID "connector_id"

//-----------------------------------------------------------------------------

ssize_t writeMsg(int fd, void* buf, size_t size)
{
    ssize_t res = -1;
    if (fd != -1) {
        res = write(fd, buf, size);
    }
    return res;
}

ssize_t writeInt(int fd, int val)
{
    char buf[16] = {0};
    int size = snprintf(buf, sizeof(buf), "%d", val);
    return writeMsg(fd, buf, static_cast<size_t>(size));
}

ssize_t readMsg(int fd, void* buf, size_t size)
{
    ssize_t res = -1;
    if (fd != -1) {
        res = read(fd, buf, size);
    }
    return res;
}

ssize_t readInt(int fd, int* val)
{
    char buf[16] = {0};
    ssize_t res = readMsg(fd, buf, sizeof(buf));
    if (res > 0)
    {
        *val = atoi(buf);
    }
    return res;
}

//-----------------------------------------------------------------------------

LedDevice& LedDevice::getInstance()
{
    static LedDevice gInstance;
    return gInstance;
}

LedDevice::LedDevice()
{
    struct dirent* ent;
    DIR* dir_led;
    dir_led = opendir(LED_PATH_PREFIX);
    if (dir_led)
    {
        for (ent = readdir(dir_led); ent != nullptr; ent = readdir(dir_led))
        {
            if (strstr(ent->d_name, LED_DIR_NAME))
            {
                struct DisplayLedState state;
                state.fd = -1;
                state.brightness = -2.0f;
                state.cur_brightness = -1;
                state.drm_id_connector = -1;
                state.directory_name = ent->d_name;

                initLedState(&state);
                m_leds.push_back(state);
            }
        }
        closedir(dir_led);
    }
}

LedDevice::~LedDevice()
{
    for (auto iter = m_leds.begin(); iter != m_leds.end(); iter++)
    {
        DisplayLedState& led = (*iter);
        if (led.fd >= 0)
        {
            protectedClose(led.fd);
        }
    }
}

void LedDevice::initLedState(DisplayLedState* state)
{
    if (state == nullptr)
    {
        return;
    }

    HWC_LOGI("%s, directory_name = %s", __FUNCTION__, state->directory_name.c_str());

    state->is_support = false;

    std::string path = std::string(LED_PATH_PREFIX) + "/" + state->directory_name + "/" + LED_NODE_MAX_BRIGHTNESS;
    ssize_t res = readIntFromPath(path.c_str(), &state->max_brightness);
    if (res <= 0)
    {
        HWC_LOGW("failed to read max brightness from %s", path.c_str());
        return;
    }

    path = std::string(LED_PATH_PREFIX) + "/" + state->directory_name + "/" + LED_NODE_MIN_BRIGHTNESS;
    res = readIntFromPath(path.c_str(), &state->min_brightness);
    if (res <= 0)
    {
        HWC_LOGW("failed to read min brightness from %s", path.c_str());
        return;
    }

    path = std::string(LED_PATH_PREFIX) + "/" + state->directory_name + "/" + LED_NODE_CONNECTOR_ID;
    res = readIntFromPath(path.c_str(), &state->drm_id_connector);
    if (res <= 0)
    {
        HWC_LOGW("failed to read connector id from %s", path.c_str());
        return;
    }

    if (state->min_brightness >= state->max_brightness)
    {
        HWC_LOGW("(%s, %d) the max and min brightness area invalid(max=%d, min=%d)",
                 state->directory_name.c_str(), state->drm_id_connector, state->max_brightness, state->min_brightness);
        return;
    }

    path = std::string(LED_PATH_PREFIX) + "/" + state->directory_name + "/" + LED_NODE_BRIGHTNESS;
    state->fd = open(path.c_str(), O_RDWR);
    if (state->fd < 0)
    {
        HWC_LOGW("(%s, %d) failed to open led path[%s]",
                 state->directory_name.c_str(), state->drm_id_connector, path.c_str());
        return;
    }

    HWC_LOGI("(%s, %d) led state: range[%d~%d]",
             state->directory_name.c_str(), state->drm_id_connector, state->min_brightness, state->max_brightness);
    state->is_support = true;
}

ssize_t LedDevice::readIntFromPath(const char* path, int* val)
{
    ssize_t size = 0;
    if (path == nullptr || val == nullptr)
    {
        HWC_LOGW("%s: invalid parameter path[%p] val[%p]", __func__, path, val);
        return size;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        HWC_LOGW("%s: failed to open path[%s]", __func__, path);
        return size;
    }

    size = readInt(fd, val);
    if (size <= 0)
    {
        HWC_LOGW("%s: failed to read path[%s][%zd]", __func__, path, size);
    }

    protectedClose(fd);
    return size;
}

int32_t LedDevice::getBrightnessSupport(uint32_t drm_id_connector, bool* support)
{
    *support = false;

    for (auto iter = m_leds.begin(); iter != m_leds.end(); iter++)
    {
        DisplayLedState& led = (*iter);
        if (led.drm_id_connector == static_cast<int>(drm_id_connector))
        {
            *support = led.is_support;
            break;
        }
    }

    return HWC2_ERROR_NONE;
}

int32_t LedDevice::setBrightness(uint32_t drm_id_connector, float brightness)
{
    DisplayLedState* led = nullptr;
    for (auto iter = m_leds.begin(); iter != m_leds.end(); iter++)
    {
        DisplayLedState& led_state = (*iter);
        if (led_state.drm_id_connector == static_cast<int>(drm_id_connector))
        {
            led = &led_state;
            break;
        }
    }

    if (led == nullptr)
    {
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (!led->is_support)
    {
        HWC_LOGW("(%d) %s: set brightness(%f) with unsupported display",
                 led->drm_id_connector, __func__, brightness);
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (led->brightness == brightness)
    {
        HWC_LOGD("(%d) set brightness with the same brightness(%f)", led->drm_id_connector, brightness);
        return HWC2_ERROR_NONE;
    }

    // max:1.0f  min:0.0f  off:-1.0f
    int val = -1;
    if (brightness == -1.0f)
    {
        val = 0;
    }
    else if (brightness < 0.0f || brightness > 1.0f)
    {
        HWC_LOGW("(%d) set brightness with invalid value(%f)", led->drm_id_connector, brightness);
        return HWC2_ERROR_BAD_PARAMETER;
    }
    else
    {
        val = static_cast<int>((led->max_brightness - led->min_brightness) * brightness);
        val += led->min_brightness;
    }

    if (val == led->cur_brightness)
    {
        HWC_LOGD("(%d) set brightness with the same config value(%d, %f->%f)",
                 led->drm_id_connector, val, led->brightness, brightness);
        return HWC2_ERROR_NONE;
    }

    ssize_t size = writeInt(led->fd, val);

    if (size <= 0)
    {
        HWC_LOGW("(%d)failed to set brightness[%d]", led->drm_id_connector, val);
        return HWC2_ERROR_NO_RESOURCES;
    }
    HWC_LOGD("(%d) set brightness[float:%f->%f | int:%d->%d]", led->drm_id_connector, led->brightness,
             brightness, led->cur_brightness, val);
    led->brightness = brightness;
    led->cur_brightness = val;

    return HWC2_ERROR_NONE;
}

void LedDevice::dump(String8* dump_str)
{
    dump_str->appendFormat("[LED state]\n");

    for (auto iter = m_leds.begin(); iter != m_leds.end(); iter++)
    {
        DisplayLedState& led = (*iter);
        dump_str->appendFormat("\tdirectory_name: %s\n\t  drm_id_connector [%d], support[%d]\n",
                               led.directory_name.c_str(), led.drm_id_connector, led.is_support);
        if (led.is_support)
        {
            dump_str->appendFormat("\t             brightness[%f] range[%d~%d] current[%d]\n",
                                   led.brightness, led.min_brightness, led.max_brightness,
                                   led.cur_brightness);
        }
    }

    dump_str->appendFormat("\n");
}
