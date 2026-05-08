#include "macos_pointer_acceleration.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const double MACOS_CURSOR_SCALE = 96.0 / 67.0;
const double MACOS_FIXED_SCALE = 65536.0;
const double MACOS_VELOCITY_FLOOR = 1.0 / MACOS_FIXED_SCALE;

struct macos_curve_fixed_t {
    int32_t index;
    int32_t gain_linear;
    int32_t gain_parabolic;
    int32_t gain_cubic;
    int32_t gain_quartic;
    int32_t tangent_linear;
    int32_t tangent_root;
};

struct macos_curve_t {
    double index;
    double gain_linear;
    double gain_parabolic;
    double gain_cubic;
    double gain_quartic;
    double tangent_linear;
    double tangent_root;
};

const macos_curve_fixed_t MACOS_ACCEL_CURVES[] = {
    { 0, 65536, 0, 0, 0, 524288, 0 },
    { 8192, 60293, 26214, 5243, 0, 537395, 1245184 },
    { 32768, 60948, 36045, 6554, 0, 543949, 1179648 },
    { 45056, 61604, 46531, 7864, 0, 550502, 1114112 },
    { 57344, 62259, 57672, 9830, 0, 557056, 1048576 },
    { 65536, 62915, 69468, 11796, 0, 563610, 983040 },
    { 98304, 63570, 81920, 14418, 0, 570163, 917504 },
    { 131072, 64225, 95027, 17695, 0, 576717, 851968 },
    { 163840, 64881, 108790, 21627, 0, 583270, 786432 },
    { 196608, 65536, 123208, 26214, 0, 589824, 786432 },
};

double fixed_to_double(int32_t value) {
    return (double) value / MACOS_FIXED_SCALE;
}

macos_curve_t curve_from_fixed(const macos_curve_fixed_t& curve) {
    return (macos_curve_t) {
        .index = fixed_to_double(curve.index),
        .gain_linear = fixed_to_double(curve.gain_linear),
        .gain_parabolic = fixed_to_double(curve.gain_parabolic),
        .gain_cubic = fixed_to_double(curve.gain_cubic),
        .gain_quartic = fixed_to_double(curve.gain_quartic),
        .tangent_linear = fixed_to_double(curve.tangent_linear),
        .tangent_root = fixed_to_double(curve.tangent_root),
    };
}

double lerp(double a, double b, double ratio) {
    return a + (b - a) * ratio;
}

double square(double value) {
    return value * value;
}

double cube(double value) {
    return value * value * value;
}

double fourth_power(double value) {
    double squared = value * value;
    return squared * squared;
}

macos_curve_t interpolate_macos_curve(double tracking_speed) {
    const size_t curve_count = sizeof(MACOS_ACCEL_CURVES) / sizeof(MACOS_ACCEL_CURVES[0]);
    macos_curve_t low = curve_from_fixed(MACOS_ACCEL_CURVES[0]);

    if (tracking_speed <= low.index) {
        return low;
    }

    for (size_t i = 1; i < curve_count; i++) {
        macos_curve_t high = curve_from_fixed(MACOS_ACCEL_CURVES[i]);
        if (tracking_speed <= high.index) {
            double span = high.index - low.index;
            if (span == 0.0) {
                return high;
            }
            double ratio = (tracking_speed - low.index) / span;
            return (macos_curve_t) {
                .index = lerp(low.index, high.index, ratio),
                .gain_linear = lerp(low.gain_linear, high.gain_linear, ratio),
                .gain_parabolic = lerp(low.gain_parabolic, high.gain_parabolic, ratio),
                .gain_cubic = lerp(low.gain_cubic, high.gain_cubic, ratio),
                .gain_quartic = lerp(low.gain_quartic, high.gain_quartic, ratio),
                .tangent_linear = lerp(low.tangent_linear, high.tangent_linear, ratio),
                .tangent_root = lerp(low.tangent_root, high.tangent_root, ratio),
            };
        }
        low = high;
    }

    return low;
}

double macos_curve_segment1(const macos_curve_t& curve, double value) {
    return curve.gain_linear * value +
           square(curve.gain_parabolic * value) +
           cube(curve.gain_cubic * value) +
           fourth_power(curve.gain_quartic * value);
}

double macos_curve_segment1_slope(const macos_curve_t& curve, double value) {
    return curve.gain_linear +
           2.0 * value * square(curve.gain_parabolic) +
           3.0 * square(value) * cube(curve.gain_cubic) +
           4.0 * cube(value) * fourth_power(curve.gain_quartic);
}

double macos_curve_value(const macos_curve_t& curve, double value) {
    double tangent0 = std::numeric_limits<double>::infinity();
    double tangent1 = std::numeric_limits<double>::infinity();
    double m0 = 0.0;
    double b0 = 0.0;
    double m1 = 0.0;
    double b1 = 0.0;

    if (curve.tangent_linear != 0.0) {
        double y0 = macos_curve_segment1(curve, curve.tangent_linear);
        m0 = macos_curve_segment1_slope(curve, curve.tangent_linear);
        b0 = y0 - m0 * curve.tangent_linear;
        tangent0 = curve.tangent_linear;

        if (curve.tangent_root != 0.0) {
            double y1 = m0 * curve.tangent_root + b0;
            m1 = 2.0 * y1 * m0;
            b1 = y1 * y1 - m1 * curve.tangent_root;
            tangent1 = curve.tangent_root;
        }
    } else if (curve.tangent_root != 0.0) {
        double y0 = macos_curve_segment1(curve, curve.tangent_root);
        m1 = macos_curve_segment1_slope(curve, curve.tangent_root);
        b1 = y0 * y0 - m1 * curve.tangent_root;
        tangent0 = curve.tangent_root;
    }

    if (value <= tangent0) {
        return macos_curve_segment1(curve, value);
    }
    if (value <= tangent1 && tangent0 == curve.tangent_linear) {
        return m0 * value + b0;
    }
    return std::sqrt(m1 * value + b1);
}

double adjusted_velocity(double velocity, const macos_pointer_acceleration_settings_t& settings) {
    double adjusted = velocity * settings.fixed_multiplier;
    return std::max(adjusted, MACOS_VELOCITY_FLOOR);
}

double accelerated_magnitude(double velocity, const macos_pointer_acceleration_settings_t& settings) {
    macos_curve_t curve = interpolate_macos_curve(settings.tracking_speed);
    double device_scale = settings.pointer_resolution / settings.frame_rate;
    double standardized = velocity / device_scale;

    return macos_curve_value(curve, standardized) * MACOS_CURSOR_SCALE;
}

}  // namespace

int16_t macos_max_first_segment_raw_delta(const macos_pointer_acceleration_settings_t& settings) {
    macos_curve_t curve = interpolate_macos_curve(settings.tracking_speed);
    double standardized_limit = curve.tangent_linear;
    if (standardized_limit <= 0.0) {
        standardized_limit = curve.tangent_root;
    }
    if (standardized_limit <= 0.0) {
        return 1;
    }

    double device_scale = settings.pointer_resolution / settings.frame_rate;
    double fixed_multiplier = std::max(settings.fixed_multiplier, MACOS_VELOCITY_FLOOR);
    double raw_limit = standardized_limit * device_scale / fixed_multiplier;

    if (raw_limit < 1.0) {
        return 1;
    }
    if (raw_limit > (double) std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    return (int16_t) std::floor(raw_limit);
}

macos_delta_t apply_macos_acceleration(int64_t dx, int64_t dy, const macos_pointer_acceleration_settings_t& settings) {
    if (dx == 0 && dy == 0) {
        return (macos_delta_t) { .dx = 0.0, .dy = 0.0 };
    }

    double raw_dx = (double) dx;
    double raw_dy = (double) dy;
    double velocity = std::floor(std::sqrt(raw_dx * raw_dx + raw_dy * raw_dy));
    velocity = std::max(velocity, MACOS_VELOCITY_FLOOR);

    double adjusted = adjusted_velocity(velocity, settings);
    double multiplier = accelerated_magnitude(adjusted, settings) / adjusted;
    return (macos_delta_t) {
        .dx = raw_dx * multiplier,
        .dy = raw_dy * multiplier,
    };
}
