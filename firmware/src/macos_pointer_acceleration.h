#ifndef _MACOS_POINTER_ACCELERATION_H_
#define _MACOS_POINTER_ACCELERATION_H_

#include <stdint.h>

struct macos_pointer_acceleration_settings_t {
    double tracking_speed;
    double pointer_resolution;
    double frame_rate;
    double fixed_multiplier;
    double report_rate;
};

struct macos_delta_t {
    double dx;
    double dy;
};

struct macos_pointer_acceleration_state_t {
    bool has_last_timestamp;
    uint64_t last_timestamp_us;
    uint32_t last_delta_us;
    double last_rate_multiplier;
    double last_velocity;
    double last_adjusted_velocity;
};

void reset_macos_acceleration_state(macos_pointer_acceleration_state_t& state);
macos_delta_t apply_macos_acceleration(int64_t dx, int64_t dy, const macos_pointer_acceleration_settings_t& settings);
macos_delta_t apply_macos_acceleration(int64_t dx, int64_t dy, const macos_pointer_acceleration_settings_t& settings, macos_pointer_acceleration_state_t& state, uint64_t timestamp_us);
int16_t macos_max_first_segment_raw_delta(const macos_pointer_acceleration_settings_t& settings);

#endif
