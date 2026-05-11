#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>

#define SCREEN_HOPPER_VENDOR_ID 0xCAFE
#define SCREEN_HOPPER_PRODUCT_ID 0xBAF3

#define CONFIG_VERSION 6
#define CONFIG_SIZE 64
#define RUNTIME_SIZE 64

#define REPORT_ID_MOUSE_ABSOLUTE 1
#define REPORT_ID_KEYBOARD_FIXED 2
#define REPORT_ID_CONSUMER_FIXED 3
#define REPORT_ID_MOUSE_RELATIVE_FIXED 4
#define REPORT_ID_MULTIPLIER 99
#define REPORT_ID_CONFIG 100
#define REPORT_ID_RUNTIME 101

#define CONFIG_USAGE_PAGE 0xFF00
#define CONFIG_USAGE 0x20
#define RUNTIME_USAGE_PAGE 0xFF01
#define RUNTIME_USAGE 0x21

#define CONFIG_FLAG_UNMAPPED_PASSTHROUGH 0x01
#define MAPPING_FLAG_STICKY 0x01

#define MOUSE_CONFIG_SCALE 65536
#define NSCREENS 2

enum class ConfigCommand : int8_t {
    NO_COMMAND = 0,
    RESET_INTO_BOOTSEL = 1,
    SET_CONFIG = 2,
    GET_CONFIG = 3,
    CLEAR_MAPPING = 4,
    ADD_MAPPING = 5,
    GET_MAPPING = 6,
    PERSIST_CONFIG = 7,
    GET_OUR_USAGES = 8,
    GET_THEIR_USAGES = 9,
    SUSPEND = 10,
    RESUME = 11,
    SET_SCREEN = 12,
    GET_SCREEN = 13,
};

enum class RuntimeCommand : int8_t {
    NO_COMMAND = 0,
    GET_STATUS = 1,
    SET_HOST_CURSOR = 2,
    SET_MOUSE_CONFIG = 3,
    GET_MOUSE_CONFIG = 4,
};

struct usage_def_t {
    uint8_t report_id;
    uint8_t size;
    uint16_t bitpos;
    bool is_relative;
    bool is_array = false;
    int32_t logical_minimum;
    uint32_t index = 0;  // for arrays
    uint32_t count = 0;  // for arrays
};

struct map_source_t {
    uint32_t usage;
    int32_t scaling = 1000;  // * 1000
    bool sticky = false;
    uint8_t layer = 0;
};

struct usage_rle_t {
    uint32_t usage;
    uint32_t count;
};

struct __attribute__((packed)) set_feature_t {
    uint8_t version;
    ConfigCommand command;
    uint8_t data[CONFIG_SIZE - 6];
    uint32_t crc32;
};

struct __attribute__((packed)) get_feature_t {
    uint8_t data[CONFIG_SIZE - 4];
    uint32_t crc32;
};

struct __attribute__((packed)) mapping_config_t {
    uint32_t target_usage;
    uint32_t source_usage;
    int32_t scaling;  // * 1000
    uint8_t layer;
    uint8_t flags;
};

enum class ConstraintMode : int8_t {
    NO_CONSTRAINT = 0,
    BOUNDING_BOX = 1,
    VISIBLE = 2,
};

struct __attribute__((packed)) screen_def_t {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t sensitivity;
};

struct __attribute__((packed)) macos_mouse_config_t {
    uint32_t tracking_speed;
    uint32_t pointer_resolution;
    uint32_t frame_rate;
    uint32_t fixed_multiplier;
    uint32_t placement_tolerance;
};

struct __attribute__((packed)) persist_config_t {
    uint8_t version;
    uint8_t flags;
    uint32_t partial_scroll_timeout;
    uint32_t mapping_count;
    uint8_t interval_override;
    ConstraintMode constraint_mode;
    uint32_t offscreen_sensitivity;
    uint32_t cursor_placement_interval_seconds;
    macos_mouse_config_t mouse_config;
    screen_def_t screens[NSCREENS];
};

struct __attribute__((packed)) get_config_t {
    uint8_t version;
    uint8_t flags;
    uint32_t partial_scroll_timeout;
    uint32_t mapping_count;
    uint32_t our_usage_count;
    uint32_t their_usage_count;
    uint8_t interval_override;
    ConstraintMode constraint_mode;
    uint32_t offscreen_sensitivity;
    uint32_t cursor_placement_interval_seconds;
    macos_mouse_config_t mouse_config;
};

struct __attribute__((packed)) set_config_t {
    uint8_t flags;
    uint32_t partial_scroll_timeout;
    uint8_t interval_override;
    ConstraintMode constraint_mode;
    uint32_t offscreen_sensitivity;
    uint32_t cursor_placement_interval_seconds;
    macos_mouse_config_t mouse_config;
};

struct __attribute__((packed)) get_indexed_t {
    uint32_t requested_index;
};

struct __attribute__((packed)) crc32_t {
    uint32_t crc32;
};

#define NUSAGES_IN_PACKET 3

struct __attribute__((packed)) usages_list_t {
    usage_rle_t usages[NUSAGES_IN_PACKET];
};

struct __attribute__((packed)) set_screen_t {
    uint8_t index;
    screen_def_t screen;
};

struct __attribute__((packed)) runtime_set_feature_t {
    uint8_t version;
    RuntimeCommand command;
    uint8_t data[RUNTIME_SIZE - 6];
    uint32_t crc32;
};

struct __attribute__((packed)) runtime_get_feature_t {
    uint8_t data[RUNTIME_SIZE - 4];
    uint32_t crc32;
};

struct __attribute__((packed)) runtime_cursor_t {
    int64_t x;
    int64_t y;
    int8_t active_screen;
};

struct __attribute__((packed)) runtime_status_t {
    runtime_cursor_t cursor;
    uint8_t placement_active;
    uint8_t placement_anchor_pending;
    macos_mouse_config_t mouse_config;
};

#endif
