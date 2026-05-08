#include <algorithm>
#include <cmath>
#include <inttypes.h>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <bsp/board.h>
#include <tusb.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio.h"

#include "config.h"
#include "crc.h"
#include "descriptor_parser.h"
#include "globals.h"
#include "macos_pointer_acceleration.h"
#include "our_descriptor.h"
#include "remapper.h"
#include "serial.h"

#define FORWARDER_UART uart1
#define FORWARDER_TX_PIN 20

const uint8_t MAPPING_FLAG_STICKY = 0x01;

const uint8_t V_RESOLUTION_BITMASK = (1 << 0);
const uint8_t H_RESOLUTION_BITMASK = (1 << 2);
const uint32_t V_SCROLL_USAGE = 0x00010038;
const uint32_t H_SCROLL_USAGE = 0x000C0238;
const uint32_t MOUSE_X_USAGE = 0x00010030;
const uint32_t MOUSE_Y_USAGE = 0x00010031;
const uint32_t SWITCH_SCREEN_USAGE = 0xFFF20001;

const uint8_t NLAYERS = 4;
const uint32_t LAYERS_USAGE_PAGE = 0xFFF10000;

const std::unordered_map<uint32_t, uint8_t> resolution_multiplier_masks = {
    { V_SCROLL_USAGE, V_RESOLUTION_BITMASK },
    { H_SCROLL_USAGE, H_RESOLUTION_BITMASK },
};

std::unordered_map<uint32_t, std::vector<map_source_t>> reverse_mapping;  // target -> sources list

std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> our_usages;  // report_id -> usage -> usage_def
std::unordered_map<uint32_t, usage_def_t> our_usages_flat;

std::vector<uint32_t> layer_triggering_stickies;
std::vector<uint64_t> sticky_usages;  // non-layer triggering, layer << 32 | usage
std::vector<uint64_t> screen_switching_usages;

// report_id -> ...
uint8_t* reports[MAX_INPUT_REPORT_ID + 1];
uint8_t* prev_reports[MAX_INPUT_REPORT_ID + 1];
uint8_t* report_masks_relative[MAX_INPUT_REPORT_ID + 1];
uint8_t* report_masks_absolute[MAX_INPUT_REPORT_ID + 1];
uint16_t report_sizes[MAX_INPUT_REPORT_ID + 1];

#define OR_BUFSIZE 8
uint8_t outgoing_reports[OR_BUFSIZE][CFG_TUD_HID_EP_BUFSIZE + 2];
bool outgoing_reports_mergeable[OR_BUFSIZE];
uint8_t or_head = 0;
uint8_t or_tail = 0;
uint8_t or_items = 0;

// We need a certain part of mapping processing (absolute->relative mappings) to
// happen exactly once per millisecond. This variable keeps track of whether we
// already did it this time around. It is set to true when we receive
// start-of-frame from USB host.
volatile bool tick_pending;

std::vector<uint8_t> report_ids;

// usage -> ...
std::unordered_map<uint32_t, int32_t> input_state;
std::unordered_map<uint32_t, int32_t> prev_input_state;
std::unordered_map<uint64_t, int32_t> sticky_state;  // layer << 32 | usage -> state
std::unordered_map<uint32_t, int32_t> accumulated;   // * 1000

std::vector<uint32_t> relative_usages;
std::unordered_set<uint32_t> relative_usage_set;

std::unordered_map<uint32_t, int32_t> accumulated_scroll;
std::unordered_map<uint32_t, uint64_t> last_scroll_timestamp;

bool led_state;
uint64_t next_print = 0;
uint32_t reports_received;
uint32_t reports_sent;
uint64_t next_periodic_cursor_placement = 0;
uint32_t scheduled_cursor_placement_interval_seconds = 0;

int64_t cursor_x = 0;
int64_t cursor_y = 0;
double cursor_fraction_x = 0.0;
double cursor_fraction_y = 0.0;

int8_t active_screen = 0;

int64_t bounds_min_x;
int64_t bounds_max_x;
int64_t bounds_min_y;
int64_t bounds_max_y;

struct cursor_placement_t {
    bool active;
    bool anchor_pending;
    int8_t screen;
    double predicted_x;
    double predicted_y;
    double target_x;
    double target_y;
};

cursor_placement_t cursor_placement = {};

int64_t consume_fractional_cursor_delta(double delta, double& fraction) {
    fraction += delta;
    int64_t whole = (int64_t) std::trunc(fraction);
    fraction -= (double) whole;
    return whole;
}

int64_t consume_scaled_axis_movement(uint32_t usage, uint32_t sensitivity) {
    if (sensitivity == 0) {
        return 0;
    }

    int32_t raw_accumulated = accumulated[usage];
    int64_t delta = (int64_t) raw_accumulated * sensitivity / 1000;
    if (delta == 0) {
        return 0;
    }

    int64_t consumed = delta * 1000 / sensitivity;
    if (consumed == 0) {
        consumed = raw_accumulated > 0 ? 1 : -1;
    }
    accumulated[usage] -= (int32_t) consumed;
    return delta;
}

int32_t handle_scroll(uint32_t source_usage, uint32_t target_usage, int32_t movement) {
    int32_t ret = 0;
    if (resolution_multiplier & resolution_multiplier_masks.at(target_usage)) {  // hi-res
        ret = movement;
    } else {  // lo-res
        if (movement != 0) {
            last_scroll_timestamp[source_usage] = time_us_64();
            accumulated_scroll[source_usage] += movement;
            int ticks = accumulated_scroll[source_usage] / (1000 * RESOLUTION_MULTIPLIER);
            accumulated_scroll[source_usage] -= ticks * (1000 * RESOLUTION_MULTIPLIER);
            ret = ticks * 1000;
        } else {
            if ((accumulated_scroll[source_usage] != 0) &&
                (time_us_64() - last_scroll_timestamp[source_usage] > partial_scroll_timeout)) {
                accumulated_scroll[source_usage] = 0;
            }
        }
    }
    return ret;
}

inline int8_t get_bit(const uint8_t* data, int len, uint16_t bitpos) {
    int byte_no = bitpos / 8;
    int bit_no = bitpos % 8;
    if (byte_no < len) {
        return (data[byte_no] & 1 << bit_no) ? 1 : 0;
    }
    return 0;
}

inline uint32_t get_bits(const uint8_t* data, int len, uint16_t bitpos, uint8_t size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) {
        value |= get_bit(data, len, bitpos + i) << i;
    }
    return value;
}

inline void put_bit(uint8_t* data, int len, uint16_t bitpos, uint8_t value) {
    int byte_no = bitpos / 8;
    int bit_no = bitpos % 8;
    if (byte_no < len) {
        data[byte_no] &= ~(1 << bit_no);
        data[byte_no] |= (value & 1) << bit_no;
    }
}

inline void put_bits(uint8_t* data, int len, uint16_t bitpos, uint8_t size, uint32_t value) {
    for (int i = 0; i < size; i++) {
        put_bit(data, len, bitpos + i, (value >> i) & 1);
    }
}

int16_t clamp_relative_axis(int64_t value) {
    if (value > std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    if (value < std::numeric_limits<int16_t>::min()) {
        return std::numeric_limits<int16_t>::min();
    }
    return (int16_t) value;
}

double abs_double(double value) {
    return value < 0.0 ? -value : value;
}

double clamp_double(double value, double low, double high) {
    return std::max(low, std::min(value, high));
}

template <typename T>
void copy_set_to_vector(const std::unordered_set<T>& input, std::vector<T>& output) {
    output.clear();
    output.reserve(input.size());
    for (T value : input) {
        output.push_back(value);
    }
}

bool queue_outgoing_report(int8_t target_screen, uint8_t report_id, const uint8_t* report, bool mergeable) {
    if (or_items == OR_BUFSIZE) {
        printf("overflow!\n");
        return false;
    }

    outgoing_reports[or_tail][0] = (uint8_t) target_screen;
    outgoing_reports[or_tail][1] = report_id;
    outgoing_reports_mergeable[or_tail] = mergeable;
    memcpy(outgoing_reports[or_tail] + 2, report, report_sizes[report_id]);
    or_tail = (or_tail + 1) % OR_BUFSIZE;
    or_items++;
    return true;
}

usage_def_t& our_usage_for_report(uint8_t report_id, uint32_t usage) {
    return our_usages[report_id][usage];
}

bool queue_mouse_absolute(int8_t target_screen, int32_t x, int32_t y) {
    usage_def_t& our_usage_x = our_usage_for_report(REPORT_ID_MOUSE, MOUSE_X_USAGE);
    usage_def_t& our_usage_y = our_usage_for_report(REPORT_ID_MOUSE, MOUSE_Y_USAGE);

    uint8_t temp_report[CFG_TUD_HID_EP_BUFSIZE];
    memset(temp_report, 0, report_sizes[REPORT_ID_MOUSE]);

    put_bits(temp_report, report_sizes[REPORT_ID_MOUSE], our_usage_x.bitpos, our_usage_x.size, (uint32_t) x);
    put_bits(temp_report, report_sizes[REPORT_ID_MOUSE], our_usage_y.bitpos, our_usage_y.size, (uint32_t) y);

    return queue_outgoing_report(target_screen, REPORT_ID_MOUSE, temp_report, false);
}

bool queue_mouse_relative(int8_t target_screen, int16_t dx, int16_t dy, bool mergeable) {
    usage_def_t& our_usage_x = our_usage_for_report(REPORT_ID_MOUSE_RELATIVE, MOUSE_X_USAGE);
    usage_def_t& our_usage_y = our_usage_for_report(REPORT_ID_MOUSE_RELATIVE, MOUSE_Y_USAGE);

    uint8_t temp_report[CFG_TUD_HID_EP_BUFSIZE];
    memset(temp_report, 0, report_sizes[REPORT_ID_MOUSE_RELATIVE]);

    put_bits(temp_report, report_sizes[REPORT_ID_MOUSE_RELATIVE], our_usage_x.bitpos, our_usage_x.size, (uint16_t) dx);
    put_bits(temp_report, report_sizes[REPORT_ID_MOUSE_RELATIVE], our_usage_y.bitpos, our_usage_y.size, (uint16_t) dy);

    return queue_outgoing_report(target_screen, REPORT_ID_MOUSE_RELATIVE, temp_report, mergeable);
}

bool needs_to_be_sent(uint8_t report_id) {
    uint8_t* report = reports[report_id];
    uint8_t* prev_report = prev_reports[report_id];
    uint8_t* relative = report_masks_relative[report_id];
    uint8_t* absolute = report_masks_absolute[report_id];

    for (int i = 0; i < report_sizes[report_id]; i++) {
        if ((report[i] & relative[i]) || ((report[i] & absolute[i]) != (prev_report[i] & absolute[i]))) {
            return true;
        }
    }
    return false;
}

void set_mapping_from_config() {
    std::unordered_set<uint32_t> layer_triggering_sticky_set;
    std::unordered_set<uint64_t> sticky_usage_set;
    std::unordered_set<uint64_t> screen_switching_usages_set;
    std::unordered_set<uint32_t> mapped;

    reverse_mapping.clear();

    for (auto const& mapping : config_mappings) {
        reverse_mapping[mapping.target_usage].push_back((map_source_t) {
            .usage = mapping.source_usage,
            .scaling = mapping.scaling,
            .sticky = (mapping.flags & MAPPING_FLAG_STICKY) != 0,
            .layer = (mapping.layer < NLAYERS) ? mapping.layer : (uint8_t) 0,
        });
        if (mapping.layer == 0) {
            mapped.insert(mapping.source_usage);
        }
        if ((mapping.flags & MAPPING_FLAG_STICKY) != 0) {
            if ((mapping.target_usage & 0xFFFF0000) == LAYERS_USAGE_PAGE) {
                layer_triggering_sticky_set.insert(mapping.source_usage);
            } else {
                sticky_usage_set.insert(((uint64_t) mapping.layer << 32) | mapping.source_usage);
            }
        }
        if (mapping.target_usage == SWITCH_SCREEN_USAGE) {
            screen_switching_usages_set.insert(((uint64_t) mapping.layer << 32) | mapping.source_usage);
        }
    }

    copy_set_to_vector(layer_triggering_sticky_set, layer_triggering_stickies);
    copy_set_to_vector(sticky_usage_set, sticky_usages);
    copy_set_to_vector(screen_switching_usages_set, screen_switching_usages);

    if (unmapped_passthrough) {
        for (auto const& [usage, usage_def] : our_usages_flat) {
            if (!mapped.count(usage)) {
                reverse_mapping[usage].push_back((map_source_t) { .usage = usage });
            }
        }
    }
}

void screens_updated() {
    bounds_min_x = screens[0].x;
    bounds_max_x = screens[0].x + screens[0].w;
    bounds_min_y = screens[0].y;
    bounds_max_y = screens[0].y + screens[0].h;
    for (uint8_t i = 1; i < NSCREENS; i++) {
        bounds_min_x = std::min(bounds_min_x, (int64_t) screens[i].x);
        bounds_max_x = std::max(bounds_max_x, (int64_t) screens[i].x + screens[i].w);
        bounds_min_y = std::min(bounds_min_y, (int64_t) screens[i].y);
        bounds_max_y = std::max(bounds_max_y, (int64_t) screens[i].y + screens[i].h);
    }

    cursor_x = screens[0].x + screens[0].w / 2;
    cursor_y = screens[0].y + screens[0].h / 2;
    cursor_fraction_x = 0.0;
    cursor_fraction_y = 0.0;
    cursor_placement.active = false;
    cursor_placement.anchor_pending = false;
    active_screen = 0;
}

bool differ_on_absolute(const uint8_t* report1, const uint8_t* report2, uint8_t report_id) {
    uint8_t* absolute = report_masks_absolute[report_id];

    for (int i = 0; i < report_sizes[report_id]; i++) {
        if ((report1[i] & absolute[i]) != (report2[i] & absolute[i])) {
            return true;
        }
    }

    return false;
}

void aggregate_relative(uint8_t* prev_report, const uint8_t* report, uint8_t report_id) {
    for (auto const& [usage, usage_def] : our_usages[report_id]) {
        if (usage_def.is_relative) {
            int32_t val1 = get_bits(report, report_sizes[report_id], usage_def.bitpos, usage_def.size);
            if (usage_def.logical_minimum < 0) {
                if (val1 & (1 << (usage_def.size - 1))) {
                    val1 |= 0xFFFFFFFF << usage_def.size;
                }
            }
            if (val1) {
                int32_t val2 = get_bits(prev_report, report_sizes[report_id], usage_def.bitpos, usage_def.size);
                if (usage_def.logical_minimum < 0) {
                    if (val2 & (1 << (usage_def.size - 1))) {
                        val2 |= 0xFFFFFFFF << usage_def.size;
                    }
                }

                put_bits(prev_report, report_sizes[report_id], usage_def.bitpos, usage_def.size, val1 + val2);
            }
        }
    }
}

bool within_bounds(int64_t x, int64_t y, int8_t& active_screen) {
    active_screen = -1;
    for (uint8_t i = 0; i < NSCREENS; i++) {
        if (screens[i].x <= x &&
            x < screens[i].x + screens[i].w &&
            screens[i].y <= y &&
            y < screens[i].y + screens[i].h) {
            active_screen = i;
            break;
        }
    }

    return ((constraint_mode == ConstraintMode::VISIBLE && active_screen != -1) ||
            (constraint_mode == ConstraintMode::BOUNDING_BOX &&
             x >= bounds_min_x &&
             x < bounds_max_x &&
             y >= bounds_min_y &&
             y < bounds_max_y) ||
            (constraint_mode == ConstraintMode::NO_CONSTRAINT));
}

void update_cursor_placement_target() {
    if ((!cursor_placement.active && !cursor_placement.anchor_pending) || cursor_placement.screen < 0) {
        return;
    }

    const screen_def_t& screen = screens[cursor_placement.screen];
    double local_x = (double) cursor_x - (double) screen.x;
    double local_y = (double) cursor_y - (double) screen.y;

    cursor_placement.target_x = clamp_double(local_x, 0.0, (double) screen.w - 1.0);
    cursor_placement.target_y = clamp_double(local_y, 0.0, (double) screen.h - 1.0);
}

void start_cursor_placement(int8_t screen) {
    cursor_placement.active = false;
    cursor_placement.anchor_pending = true;
    cursor_placement.screen = screen;
    cursor_placement.predicted_x = 0.0;
    cursor_placement.predicted_y = 0.0;
    cursor_placement.target_x = 0.0;
    cursor_placement.target_y = 0.0;
    update_cursor_placement_target();
}

uint64_t cursor_placement_interval_us() {
    return (uint64_t) cursor_placement_interval_seconds * 1000000;
}

void schedule_periodic_cursor_placement(uint64_t now) {
    scheduled_cursor_placement_interval_seconds = cursor_placement_interval_seconds;
    next_periodic_cursor_placement = now + cursor_placement_interval_us();
}

bool periodic_cursor_placement_due() {
    uint64_t now = time_us_64();
    if (cursor_placement_interval_seconds == 0) {
        next_periodic_cursor_placement = 0;
        scheduled_cursor_placement_interval_seconds = 0;
        return false;
    }

    if (scheduled_cursor_placement_interval_seconds != cursor_placement_interval_seconds ||
        next_periodic_cursor_placement == 0) {
        schedule_periodic_cursor_placement(now);
        return false;
    }

    if (now < next_periodic_cursor_placement) {
        return false;
    }

    schedule_periodic_cursor_placement(now);
    return active_screen != -1 && !cursor_placement.active && !cursor_placement.anchor_pending;
}

double predicted_placement_axis_delta(int16_t raw_delta, bool x_axis) {
    macos_delta_t accelerated = x_axis
                                    ? apply_macos_acceleration(raw_delta, 0, macos_pointer_acceleration)
                                    : apply_macos_acceleration(0, raw_delta, macos_pointer_acceleration);
    return x_axis ? accelerated.dx : accelerated.dy;
}

int16_t choose_placement_axis_step(double remaining, bool x_axis) {
    if (abs_double(remaining) <= macos_placement_tolerance) {
        return 0;
    }

    int sign = remaining > 0.0 ? 1 : -1;
    int16_t best_step = 0;
    double best_error = abs_double(remaining);
    int16_t max_step = macos_max_first_segment_raw_delta(macos_pointer_acceleration);

    for (int magnitude = 1; magnitude <= max_step; magnitude++) {
        int16_t step = (int16_t) (sign * magnitude);
        double predicted = predicted_placement_axis_delta(step, x_axis);
        double error = abs_double(remaining - predicted);
        if (error < best_error) {
            best_error = error;
            best_step = step;
        }
    }

    return best_step;
}

void emit_cursor_placement_reports() {
    if (!cursor_placement.active && !cursor_placement.anchor_pending) {
        return;
    }

    if (cursor_placement.anchor_pending) {
        if (!queue_mouse_absolute(cursor_placement.screen, 0, 0)) {
            return;
        }
        cursor_placement.anchor_pending = false;
        cursor_placement.active = true;
    }

    while (cursor_placement.active && or_items < OR_BUFSIZE - 1) {
        double remaining_x = cursor_placement.target_x - cursor_placement.predicted_x;
        double remaining_y = cursor_placement.target_y - cursor_placement.predicted_y;

        if (abs_double(remaining_x) <= macos_placement_tolerance &&
            abs_double(remaining_y) <= macos_placement_tolerance) {
            cursor_placement.active = false;
            break;
        }

        bool use_x = abs_double(remaining_x) >= abs_double(remaining_y);
        int16_t dx = use_x ? choose_placement_axis_step(remaining_x, true) : 0;
        int16_t dy = use_x ? 0 : choose_placement_axis_step(remaining_y, false);

        if (dx == 0 && dy == 0) {
            dx = choose_placement_axis_step(remaining_x, true);
            dy = dx == 0 ? choose_placement_axis_step(remaining_y, false) : 0;
        }
        if (dx == 0 && dy == 0) {
            cursor_placement.active = false;
            break;
        }

        if (!queue_mouse_relative(cursor_placement.screen, dx, dy, false)) {
            break;
        }

        macos_delta_t accelerated = apply_macos_acceleration(dx, dy, macos_pointer_acceleration);
        cursor_placement.predicted_x += accelerated.dx;
        cursor_placement.predicted_y += accelerated.dy;
    }
}

runtime_cursor_t get_runtime_cursor() {
    return (runtime_cursor_t) {
        .x = cursor_x,
        .y = cursor_y,
        .active_screen = active_screen,
    };
}

void get_runtime_placement_flags(uint8_t& placement_active, uint8_t& placement_anchor_pending) {
    placement_active = cursor_placement.active ? 1 : 0;
    placement_anchor_pending = cursor_placement.anchor_pending ? 1 : 0;
}

void set_cursor_from_host(const runtime_cursor_t& cursor) {
    cursor_x = cursor.x;
    cursor_y = cursor.y;
    cursor_fraction_x = 0.0;
    cursor_fraction_y = 0.0;
    cursor_placement.active = false;
    cursor_placement.anchor_pending = false;

    if (cursor.active_screen >= 0 && cursor.active_screen < NSCREENS) {
        active_screen = cursor.active_screen;
        return;
    }

    int8_t derived_active_screen = -1;
    within_bounds(cursor_x, cursor_y, derived_active_screen);
    active_screen = derived_active_screen;
}

void process_mapping(bool auto_repeat) {
    if (suspended) {
        return;
    }

    for (auto const& usage : layer_triggering_stickies) {
        if ((prev_input_state[usage] == 0) && (input_state[usage] != 0)) {
            sticky_state[usage] = !sticky_state[usage];
        }
        prev_input_state[usage] = input_state[usage];
    }

    static bool layer_state[NLAYERS];
    // layer triggers work on all layers (no matter what layer they are defined on)
    // they can be sticky
    layer_state[0] = true;
    for (int i = 1; i < NLAYERS; i++) {
        layer_state[i] = false;
        for (auto const& map_source : reverse_mapping[LAYERS_USAGE_PAGE | i]) {
            if (map_source.sticky ? sticky_state[map_source.usage] : input_state[map_source.usage]) {
                layer_state[i] = true;
                layer_state[0] = false;
                break;
            }
        }
    }

    for (auto const& layer_usage : sticky_usages) {
        uint32_t usage = layer_usage & 0xFFFFFFFF;
        uint32_t layer = layer_usage >> 32;
        if (layer_state[layer]) {
            if ((prev_input_state[usage] == 0) && (input_state[usage] != 0)) {
                sticky_state[layer_usage] = !sticky_state[layer_usage];
            }
        }
        prev_input_state[usage] = input_state[usage];
    }

    bool manual_screen_changed = false;
    for (auto const& layer_usage : screen_switching_usages) {
        uint32_t usage = layer_usage & 0xFFFFFFFF;
        uint32_t layer = layer_usage >> 32;
        if (layer_state[layer]) {
            if ((prev_input_state[usage] == 0) && (input_state[usage] != 0)) {
                active_screen = (active_screen + 1) % NSCREENS;
                cursor_x = screens[active_screen].x + screens[active_screen].w / 2;
                cursor_y = screens[active_screen].y + screens[active_screen].h / 2;
                cursor_fraction_x = 0.0;
                cursor_fraction_y = 0.0;
                manual_screen_changed = true;
            }
        }
        prev_input_state[usage] = input_state[usage];
    }

    for (auto const& [target, sources] : reverse_mapping) {
        auto search = our_usages_flat.find(target);
        if (search == our_usages_flat.end()) {
            continue;
        }
        const usage_def_t& our_usage = search->second;
        if (our_usage.is_relative || target == MOUSE_X_USAGE || target == MOUSE_Y_USAGE) {
            for (auto const& map_source : sources) {
                bool source_is_relative = relative_usage_set.count(map_source.usage);
                if (auto_repeat || source_is_relative) {
                    int32_t value = 0;
                    if (map_source.sticky) {
                        value = sticky_state[((uint64_t) map_source.layer << 32) | map_source.usage] * map_source.scaling;
                    } else {
                        if (layer_state[map_source.layer]) {
                            value = (source_is_relative
                                         ? input_state[map_source.usage]
                                         : !!input_state[map_source.usage]) *
                                    map_source.scaling;
                        }
                    }
                    if (value != 0) {
                        if (target == V_SCROLL_USAGE || target == H_SCROLL_USAGE) {
                            accumulated[target] += handle_scroll(map_source.usage, target, value * RESOLUTION_MULTIPLIER);
                        } else {
                            accumulated[target] += value;
                        }
                    }
                }
            }
        } else {
            int32_t value = 0;
            for (auto const& map_source : sources) {
                if (map_source.sticky && (sticky_state[((uint64_t) map_source.layer << 32) | map_source.usage] != 0)) {
                    value = sticky_state[((uint64_t) map_source.layer << 32) | map_source.usage];
                } else {
                    if ((layer_state[map_source.layer]) &&
                        (relative_usage_set.count(map_source.usage)
                             ? (input_state[map_source.usage] * map_source.scaling > 0)
                             : input_state[map_source.usage])) {
                        value = 1;
                    }
                }
            }
            if (value) {
                put_bits((uint8_t*) reports[our_usage.report_id], report_sizes[our_usage.report_id], our_usage.bitpos, our_usage.size, value);
            }
        }
    }

    for (auto usage : relative_usages) {
        input_state[usage] = 0;
    }

    int64_t dx = consume_scaled_axis_movement(MOUSE_X_USAGE, screens[active_screen].sensitivity);
    int64_t dy = consume_scaled_axis_movement(MOUSE_Y_USAGE, screens[active_screen].sensitivity);

    // Apple accelerates the vector magnitude once, then applies that scalar to both axes.
    macos_delta_t accelerated = apply_macos_acceleration(dx, dy, macos_pointer_acceleration);
    int64_t accelerated_dx = consume_fractional_cursor_delta(accelerated.dx, cursor_fraction_x);
    int64_t accelerated_dy = consume_fractional_cursor_delta(accelerated.dy, cursor_fraction_y);

    int64_t new_cursor_x = cursor_x + accelerated_dx;
    int64_t new_cursor_y = cursor_y + accelerated_dy;

    int8_t new_active_screen;
    bool screen_changed = manual_screen_changed;
    if (within_bounds(new_cursor_x, new_cursor_y, new_active_screen)) {
        cursor_x = new_cursor_x;
        cursor_y = new_cursor_y;
        if (new_active_screen != active_screen) {
            screen_changed = true;
        }
        active_screen = new_active_screen;
    } else if (within_bounds(cursor_x, new_cursor_y, new_active_screen)) {  // so that the cursor doesn't snag on screen edges
        cursor_y = new_cursor_y;
        if (new_active_screen != active_screen) {
            screen_changed = true;
        }
        active_screen = new_active_screen;
    } else if (within_bounds(new_cursor_x, cursor_y, new_active_screen)) {
        cursor_x = new_cursor_x;
        if (new_active_screen != active_screen) {
            screen_changed = true;
        }
        active_screen = new_active_screen;
    }

    bool movement_absorbed_by_placement = false;

    // If the target host changes, anchor at its absolute origin and then place
    // the cursor with small relative packets. Those packets are deliberately not
    // mergeable; a merged packet would be accelerated as one larger movement.
    bool periodic_placement_due = periodic_cursor_placement_due();

    if (screen_changed && active_screen != -1) {
        cursor_fraction_x = 0.0;
        cursor_fraction_y = 0.0;
        start_cursor_placement(active_screen);
        movement_absorbed_by_placement = true;
    } else if ((cursor_placement.active || cursor_placement.anchor_pending) && active_screen == cursor_placement.screen) {
        update_cursor_placement_target();
        movement_absorbed_by_placement = true;
    } else if (periodic_placement_due) {
        cursor_fraction_x = 0.0;
        cursor_fraction_y = 0.0;
        start_cursor_placement(active_screen);
        movement_absorbed_by_placement = true;
    } else if (cursor_placement.active || cursor_placement.anchor_pending) {
        cursor_placement.active = false;
        cursor_placement.anchor_pending = false;
    }

    emit_cursor_placement_reports();

    // Prepare relative movement report (always use REPORT_ID_MOUSE_RELATIVE for cursor movement)
    // Send raw dx/dy (not accelerated) - macOS will apply its own acceleration
    if (active_screen != -1 && !movement_absorbed_by_placement && (dx != 0 || dy != 0)) {
        queue_mouse_relative(active_screen, clamp_relative_axis(dx), clamp_relative_axis(dy), true);
    }

    // Handle buttons and scrolling via REPORT_ID_MOUSE_RELATIVE
    for (auto& [usage, accumulated_val] : accumulated) {
        if (accumulated_val == 0) {
            continue;
        }

        // Only process scroll/button usages, not X/Y
        if (usage == MOUSE_X_USAGE || usage == MOUSE_Y_USAGE) {
            continue;
        }

        usage_def_t& our_usage = our_usages_flat[usage];
        int32_t existing_val = get_bits((uint8_t*) reports[REPORT_ID_MOUSE_RELATIVE], report_sizes[REPORT_ID_MOUSE_RELATIVE], our_usage.bitpos, our_usage.size);
        if (our_usage.logical_minimum < 0) {
            if (existing_val & (1 << (our_usage.size - 1))) {
                existing_val |= 0xFFFFFFFF << our_usage.size;
            }
        }
        int32_t truncated = accumulated_val / 1000;
        accumulated_val -= truncated * 1000;
        if (truncated != 0) {
            put_bits((uint8_t*) reports[REPORT_ID_MOUSE_RELATIVE], report_sizes[REPORT_ID_MOUSE_RELATIVE], our_usage.bitpos, our_usage.size, existing_val + truncated);
        }
    }

    // Send any pending button/scroll reports via REPORT_ID_MOUSE_RELATIVE
    if ((active_screen != -1) && needs_to_be_sent(REPORT_ID_MOUSE_RELATIVE)) {
        if (or_items == OR_BUFSIZE) {
            printf("overflow!\n");
        } else {
            uint8_t prev = (or_tail + OR_BUFSIZE - 1) % OR_BUFSIZE;
            if ((or_items > 0) &&
                outgoing_reports_mergeable[prev] &&
                (outgoing_reports[prev][0] == active_screen) &&
                (outgoing_reports[prev][1] == REPORT_ID_MOUSE_RELATIVE) &&
                !differ_on_absolute(outgoing_reports[prev] + 2, reports[REPORT_ID_MOUSE_RELATIVE], REPORT_ID_MOUSE_RELATIVE)) {
                aggregate_relative(outgoing_reports[prev] + 2, reports[REPORT_ID_MOUSE_RELATIVE], REPORT_ID_MOUSE_RELATIVE);
            } else {
                if (queue_outgoing_report(active_screen, REPORT_ID_MOUSE_RELATIVE, reports[REPORT_ID_MOUSE_RELATIVE], true)) {
                    memcpy(prev_reports[REPORT_ID_MOUSE_RELATIVE], reports[REPORT_ID_MOUSE_RELATIVE], report_sizes[REPORT_ID_MOUSE_RELATIVE]);
                }
            }
        }
    }

    // Send keyboard and consumer reports (non-mouse)
    for (uint i = 0; i < report_ids.size(); i++) {
        uint8_t report_id = report_ids[i];
        if (report_id == REPORT_ID_MOUSE || report_id == REPORT_ID_MOUSE_RELATIVE) {
            continue;
        }
        if ((active_screen != -1) && needs_to_be_sent(report_id)) {
            if (or_items == OR_BUFSIZE) {
                printf("overflow!\n");
                break;
            }
            uint8_t prev = (or_tail + OR_BUFSIZE - 1) % OR_BUFSIZE;
            if ((or_items > 0) &&
                outgoing_reports_mergeable[prev] &&
                (outgoing_reports[prev][0] == active_screen) &&
                (outgoing_reports[prev][1] == report_id) &&
                !differ_on_absolute(outgoing_reports[prev] + 2, reports[report_id], report_id)) {
                aggregate_relative(outgoing_reports[prev] + 2, reports[report_id], report_id);
            } else {
                if (queue_outgoing_report(active_screen, report_id, reports[report_id], true)) {
                    memcpy(prev_reports[report_id], reports[report_id], report_sizes[report_id]);
                }
            }
        }
        memset(reports[report_id], 0, report_sizes[report_id]);
    }

    memset(reports[REPORT_ID_MOUSE], 0, report_sizes[REPORT_ID_MOUSE]);
    memset(reports[REPORT_ID_MOUSE_RELATIVE], 0, report_sizes[REPORT_ID_MOUSE_RELATIVE]);
}

void send_report() {
    if (suspended || (or_items == 0)) {
        return;
    }

    uint8_t target_screen = outgoing_reports[or_head][0];
    uint8_t report_id = outgoing_reports[or_head][1];

    if (target_screen == 0) {
        tud_hid_report(report_id, outgoing_reports[or_head] + 2, report_sizes[report_id]);
    } else {
        serial_write(outgoing_reports[or_head] + 1, report_sizes[report_id] + 1, FORWARDER_UART);
    }

    or_head = (or_head + 1) % OR_BUFSIZE;
    or_items--;

    reports_sent++;
}

inline void read_input(const uint8_t* report, int len, uint32_t source_usage, const usage_def_t& their_usage, uint16_t interface) {
    int32_t value = 0;
    if (their_usage.is_array) {
        for (uint i = 0; i < their_usage.count; i++) {
            if (get_bits(report, len, their_usage.bitpos + i * their_usage.size, their_usage.size) == their_usage.index) {
                value = 1;
                break;
            }
        }
    } else {
        value = get_bits(report, len, their_usage.bitpos, their_usage.size);
        if (their_usage.logical_minimum < 0) {
            if (value & (1 << (their_usage.size - 1))) {
                value |= 0xFFFFFFFF << their_usage.size;
            }
        }
    }

    if (their_usage.is_relative) {
        input_state[source_usage] = value;
    } else {
        if (value) {
            input_state[source_usage] |= 1 << interface_index[interface];
        } else {
            input_state[source_usage] &= ~(1 << interface_index[interface]);
        }
    }
}

void handle_received_report(const uint8_t* report, int len, uint16_t interface) {
    led_state = !led_state;
    board_led_write(led_state);
    reports_received++;

    mutex_enter_blocking(&their_usages_mutex);

    uint8_t report_id = 0;
    if (has_report_id_theirs[interface]) {
        report_id = report[0];
        report++;
        len--;
    }

    for (auto const& [their_usage, their_usage_def] : their_usages[interface][report_id]) {
        read_input(report, len, their_usage, their_usage_def, interface);
    }

    mutex_exit(&their_usages_mutex);
}

void rlencode(const std::set<uint32_t>& usages, std::vector<usage_rle_t>& output) {
    uint32_t start_usage = 0;
    uint32_t count = 0;
    for (auto const& usage : usages) {
        if (start_usage == 0) {
            start_usage = usage;
            count = 1;
            continue;
        }
        if (usage == start_usage + count) {
            count++;
        } else {
            output.push_back({ .usage = start_usage, .count = count });
            start_usage = usage;
            count = 1;
        }
    }
    if (start_usage != 0) {
        output.push_back({ .usage = start_usage, .count = count });
    }
}

void update_their_descriptor_derivates() {
    relative_usages.clear();
    relative_usage_set.clear();
    std::set<uint32_t> their_usages_set;
    for (auto const& [interface, report_id_usage_map] : their_usages) {
        for (auto const& [report_id, usage_map] : report_id_usage_map) {
            for (auto const& [usage, usage_def] : usage_map) {
                their_usages_set.insert(usage);
                if (usage_def.is_relative) {
                    relative_usages.push_back(usage);
                    relative_usage_set.insert(usage);
                }
            }
        }
    }

    their_usages_rle.clear();
    rlencode(their_usages_set, their_usages_rle);
}

void parse_our_descriptor() {
    bool has_report_id_ours;
    std::unordered_map<uint8_t, uint16_t> report_sizes_map = parse_descriptor(our_usages, has_report_id_ours, our_report_descriptor, our_report_descriptor_length);
    for (auto const& [report_id, size] : report_sizes_map) {
        report_sizes[report_id] = size;
        reports[report_id] = new uint8_t[size];
        memset(reports[report_id], 0, size);
        prev_reports[report_id] = new uint8_t[size];
        memset(prev_reports[report_id], 0, size);
        report_masks_relative[report_id] = new uint8_t[size];
        memset(report_masks_relative[report_id], 0, size);
        report_masks_absolute[report_id] = new uint8_t[size];
        memset(report_masks_absolute[report_id], 0, size);

        report_ids.push_back(report_id);
    }

    std::set<uint32_t> our_usages_set;
    for (auto const& [report_id, usage_map] : our_usages) {
        for (auto const& [usage, usage_def] : usage_map) {
            if (!our_usages_flat.count(usage) || report_id == REPORT_ID_MOUSE_RELATIVE) {
                our_usages_flat[usage] = usage_def;
            }
            our_usages_set.insert(usage);

            if (usage_def.is_relative) {
                put_bits(report_masks_relative[report_id], report_sizes[report_id], usage_def.bitpos, usage_def.size, 0xFFFFFFFF);
            } else {
                put_bits(report_masks_absolute[report_id], report_sizes[report_id], usage_def.bitpos, usage_def.size, 0xFFFFFFFF);
            }
        }
    }

    rlencode(our_usages_set, our_usages_rle);
}

void print_stats() {
    uint64_t now = time_us_64();
    if (now > next_print) {
        printf("%" PRIu32 " %" PRIu32 "\n", reports_received, reports_sent);
        reports_received = 0;
        reports_sent = 0;
        while (next_print < now) {
            next_print += 1000000;
        }
    }
}

inline bool get_and_clear_tick_pending() {
    // atomicity not critical
    uint8_t tmp = tick_pending;
    tick_pending = false;
    return tmp;
}

void sof_handler(uint32_t frame_count) {
    tick_pending = true;
}

void forwarder_serial_init() {
    uart_init(FORWARDER_UART, FORWARDER_BAUDRATE);
    uart_set_translate_crlf(FORWARDER_UART, false);
    gpio_set_function(FORWARDER_TX_PIN, GPIO_FUNC_UART);
}

int main() {
    mutex_init(&their_usages_mutex);
    extra_init();
    forwarder_serial_init();
    parse_our_descriptor();
    load_config();
    board_init();
    tusb_init();

    tud_sof_isr_set(sof_handler);

    next_print = time_us_64() + 1000000;

    while (true) {
        if (read_report()) {
            process_mapping(get_and_clear_tick_pending());
        }
        tud_task();
        if (tud_hid_ready()) {
            if (get_and_clear_tick_pending()) {
                process_mapping(true);
            }
            send_report();
        }

        if (their_descriptor_updated) {
            update_their_descriptor_derivates();
            their_descriptor_updated = false;
        }
        if (need_to_persist_config) {
            persist_config();
            need_to_persist_config = false;
        }

        print_stats();
    }

    return 0;
}
