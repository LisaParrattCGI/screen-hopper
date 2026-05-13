#ifndef _MACOS_POINTER_ACCELERATION_H_
#define _MACOS_POINTER_ACCELERATION_H_

#include <stdint.h>

struct macos_pointer_acceleration_settings_t {
    double tracking_speed;
    double pointer_resolution;
    double frame_rate;
    double fixed_multiplier;
};

struct macos_delta_t {
    double dx;
    double dy;
};

macos_delta_t apply_macos_acceleration(int64_t dx, int64_t dy, const macos_pointer_acceleration_settings_t& settings);
int16_t macos_max_first_segment_raw_delta(const macos_pointer_acceleration_settings_t& settings);

#endif
