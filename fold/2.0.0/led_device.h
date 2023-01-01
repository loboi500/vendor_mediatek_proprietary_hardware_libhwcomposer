#pragma once

#include <utils/RefBase.h>
#include <utils/String8.h>

#include "hwc2_defs.h"
#include "display.h"

struct DisplayLedState {
    int fd;
    bool is_support;
    float brightness;
    int cur_brightness;
    int max_brightness;
    int min_brightness;
    int drm_id_connector;
    std::string directory_name;
};

class LedDevice : public RefBase
{
public:
    static LedDevice& getInstance();
    ~LedDevice();

    // getBrightnessSupport() is used to get the brightness capability which can adjust backlight
    int32_t getBrightnessSupport(uint32_t drm_id_connector, bool* support);

    // setBrightness() is used to change the brightness
    int32_t setBrightness(uint32_t drm_id_connector, float brightness);

    // print the led state
    void dump(String8* dump_str);

private:
    LedDevice();

    // initialize the led state with the assigned path of device node
    void initLedState(DisplayLedState* state);

    // read a integer from the assigned path
    ssize_t readIntFromPath(const char* path, int* val);

private:
    std::vector<DisplayLedState> m_leds;
};
