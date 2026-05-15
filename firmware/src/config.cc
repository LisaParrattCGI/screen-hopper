#include <cmath>
#include <unordered_set>

#include <bsp/board.h>
#include <tusb.h>

#include <pico/bootrom.h>
#include <pico/stdlib.h>

#include <hardware/flash.h>

#include "config.h"
#include "crc.h"
#include "globals.h"
#include "interval_override.h"
#include "our_descriptor.h"
#include "remapper.h"

const uint32_t PRESUMED_FLASH_SIZE = 2097152;
const uint32_t CONFIG_OFFSET_IN_FLASH = (PRESUMED_FLASH_SIZE - FLASH_SECTOR_SIZE);
const uint8_t* FLASH_CONFIG_IN_MEMORY = (((uint8_t*) XIP_BASE) + CONFIG_OFFSET_IN_FLASH);

ConfigCommand last_config_command = ConfigCommand::NO_COMMAND;
RuntimeCommand last_runtime_command = RuntimeCommand::GET_STATUS;
uint32_t requested_index = 0;
macos_mouse_config_t persistent_mouse_config = {
    .tracking_speed = 45056,
    .pointer_resolution = ADVERTISED_POINTER_RESOLUTION_FIXED,
    .frame_rate = 4390912,
    .fixed_multiplier = 65536,
    .placement_tolerance = 32768,
};

bool checksum_ok(const uint8_t* buffer, uint16_t data_size) {
    return crc32(buffer, data_size - 4) == ((crc32_t*) (buffer + data_size - 4))->crc32;
}

bool version_ok(const uint8_t* buffer) {
    return ((set_feature_t*) buffer)->version == CONFIG_VERSION;
}

double fixed16_to_double(uint32_t value) {
    return (double) value / MOUSE_CONFIG_SCALE;
}

uint32_t double_to_fixed16(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }

    double scaled = value * MOUSE_CONFIG_SCALE + 0.5;
    if (scaled >= (double) UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t) scaled;
}

uint32_t max_persisted_mapping_count() {
    return (FLASH_SECTOR_SIZE - sizeof(persist_config_t) - sizeof(crc32_t)) / sizeof(mapping_config_t);
}

bool constraint_mode_ok(ConstraintMode mode) {
    int8_t value = (int8_t) mode;
    return value >= (int8_t) ConstraintMode::NO_CONSTRAINT && value <= (int8_t) ConstraintMode::VISIBLE;
}

bool screen_ok(const screen_def_t& screen) {
    return screen.w > 0 && screen.h > 0 && screen.sensitivity > 0;
}

void set_interval_override_checked(uint8_t value) {
    uint8_t prev_interval_override = interval_override;
    interval_override = value;
    if (prev_interval_override != interval_override) {
        interval_override_updated();
    }
}

void apply_mouse_config(const macos_mouse_config_t* config) {
    macos_pointer_acceleration.tracking_speed = fixed16_to_double(config->tracking_speed);

    macos_pointer_acceleration.pointer_resolution = fixed16_to_double(config->pointer_resolution);
    if (macos_pointer_acceleration.pointer_resolution < 1.0) {
        macos_pointer_acceleration.pointer_resolution = 1.0;
    }

    macos_pointer_acceleration.frame_rate = fixed16_to_double(config->frame_rate);
    if (macos_pointer_acceleration.frame_rate < 1.0) {
        macos_pointer_acceleration.frame_rate = 1.0;
    }

    macos_pointer_acceleration.fixed_multiplier = fixed16_to_double(config->fixed_multiplier);
    if (macos_pointer_acceleration.fixed_multiplier < (1.0 / MOUSE_CONFIG_SCALE)) {
        macos_pointer_acceleration.fixed_multiplier = 1.0 / MOUSE_CONFIG_SCALE;
    }

    macos_placement_tolerance = fixed16_to_double(config->placement_tolerance);
}

void fill_mouse_config(macos_mouse_config_t* config) {
    config->tracking_speed = double_to_fixed16(macos_pointer_acceleration.tracking_speed);
    config->pointer_resolution = double_to_fixed16(macos_pointer_acceleration.pointer_resolution);
    config->frame_rate = double_to_fixed16(macos_pointer_acceleration.frame_rate);
    config->fixed_multiplier = double_to_fixed16(macos_pointer_acceleration.fixed_multiplier);
    config->placement_tolerance = double_to_fixed16(macos_placement_tolerance);
}

void fill_runtime_status(runtime_status_t* status) {
    memset(status, 0, sizeof(runtime_status_t));
    status->cursor = get_runtime_cursor();
    get_runtime_placement_flags(status->placement_active, status->placement_anchor_pending);
    fill_mouse_config(&status->mouse_config);
}

void fill_runtime_diagnostics(runtime_diagnostics_t* diagnostics) {
    memset(diagnostics, 0, sizeof(runtime_diagnostics_t));
    *diagnostics = get_runtime_diagnostics();
}

void load_config() {
    if (checksum_ok(FLASH_CONFIG_IN_MEMORY, FLASH_SECTOR_SIZE) && version_ok(FLASH_CONFIG_IN_MEMORY)) {
        persist_config_t* config = (persist_config_t*) FLASH_CONFIG_IN_MEMORY;
        unmapped_passthrough = (config->flags & CONFIG_FLAG_UNMAPPED_PASSTHROUGH) != 0;
        if (config->partial_scroll_timeout > 0) {
            partial_scroll_timeout = config->partial_scroll_timeout;
        }
        set_interval_override_checked(config->interval_override);
        if (constraint_mode_ok(config->constraint_mode)) {
            constraint_mode = config->constraint_mode;
        }
        if (config->offscreen_sensitivity > 0) {
            screens[-1].sensitivity = config->offscreen_sensitivity;
        }
        cursor_placement_interval_seconds = config->cursor_placement_interval_seconds;
        persistent_mouse_config = config->mouse_config;
        apply_mouse_config(&persistent_mouse_config);
        fill_mouse_config(&persistent_mouse_config);
        for (uint8_t i = 0; i < NSCREENS; i++) {
            if (screen_ok(config->screens[i])) {
                screens[i] = config->screens[i];
            }
        }
        mapping_config_t* buffer_mappings = (mapping_config_t*) (FLASH_CONFIG_IN_MEMORY + sizeof(persist_config_t));
        uint32_t mapping_count = config->mapping_count;
        if (mapping_count > max_persisted_mapping_count()) {
            mapping_count = max_persisted_mapping_count();
        }
        for (uint32_t i = 0; i < mapping_count; i++) {
            config_mappings.push_back(buffer_mappings[i]);
        }
    }
    screens_updated();
    set_mapping_from_config();
}

void fill_get_config(get_config_t* config) {
    config->version = CONFIG_VERSION;
    config->flags = 0;
    if (unmapped_passthrough) {
        config->flags |= CONFIG_FLAG_UNMAPPED_PASSTHROUGH;
    }
    config->partial_scroll_timeout = partial_scroll_timeout;
    config->mapping_count = config_mappings.size();
    if (config->mapping_count > max_persisted_mapping_count()) {
        config->mapping_count = max_persisted_mapping_count();
    }
    config->our_usage_count = our_usages_rle.size();
    config->their_usage_count = their_usages_rle.size();
    config->interval_override = interval_override;
    config->constraint_mode = constraint_mode;
    config->offscreen_sensitivity = screens[-1].sensitivity;
    config->cursor_placement_interval_seconds = cursor_placement_interval_seconds;
    config->mouse_config = persistent_mouse_config;
}

void fill_persist_config(persist_config_t* config) {
    config->version = CONFIG_VERSION;
    config->flags = 0;
    if (unmapped_passthrough) {
        config->flags |= CONFIG_FLAG_UNMAPPED_PASSTHROUGH;
    }
    config->partial_scroll_timeout = partial_scroll_timeout;
    config->mapping_count = config_mappings.size();
    config->interval_override = interval_override;
    config->constraint_mode = constraint_mode;
    config->offscreen_sensitivity = screens[-1].sensitivity;
    config->cursor_placement_interval_seconds = cursor_placement_interval_seconds;
    config->mouse_config = persistent_mouse_config;
    for (uint8_t i = 0; i < NSCREENS; i++) {
        config->screens[i] = screens[i];
    }
}

void persist_config() {
    // stack size is 2KB
    static uint8_t buffer[FLASH_SECTOR_SIZE];
    memset(buffer, 0, sizeof(buffer));

    persist_config_t* config = (persist_config_t*) buffer;
    fill_persist_config(config);
    mapping_config_t* buffer_mappings = (mapping_config_t*) (buffer + sizeof(persist_config_t));
    for (uint32_t i = 0; i < config->mapping_count; i++) {
        buffer_mappings[i] = config_mappings[i];
    }

    ((crc32_t*) (buffer + FLASH_SECTOR_SIZE - 4))->crc32 = crc32(buffer, FLASH_SECTOR_SIZE - 4);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_OFFSET_IN_FLASH, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_OFFSET_IN_FLASH, buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

void tud_mount_cb() {
    // reset hi-res scroll for when we reboot from Windows into Linux
    resolution_multiplier = 0;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    if (report_id == REPORT_ID_MULTIPLIER && reqlen >= 1) {
        memcpy(buffer, &resolution_multiplier, 1);
        return 1;
    }
    if (report_id == REPORT_ID_CONFIG && reqlen >= CONFIG_SIZE) {
        get_feature_t* config_buffer = (get_feature_t*) buffer;
        memset(config_buffer, 0, sizeof(get_feature_t));
        switch (last_config_command) {
            case ConfigCommand::GET_CONFIG: {
                fill_get_config((get_config_t*) config_buffer);
                break;
            }
            case ConfigCommand::GET_MAPPING: {
                mapping_config_t* mapping_config = (mapping_config_t*) config_buffer;
                if (requested_index < config_mappings.size()) {
                    *mapping_config = config_mappings[requested_index];
                }
                break;
            }
            case ConfigCommand::GET_OUR_USAGES: {
                usages_list_t* returned_usages = (usages_list_t*) config_buffer;
                for (uint32_t i = 0; (i < NUSAGES_IN_PACKET) && (requested_index + i < our_usages_rle.size()); i++) {
                    returned_usages->usages[i] = our_usages_rle[requested_index + i];
                }
                break;
            }
            case ConfigCommand::GET_THEIR_USAGES: {
                usages_list_t* returned_usages = (usages_list_t*) config_buffer;
                for (uint32_t i = 0; (i < NUSAGES_IN_PACKET) && (requested_index + i < their_usages_rle.size()); i++) {
                    returned_usages->usages[i] = their_usages_rle[requested_index + i];
                }
                break;
            }
            case ConfigCommand::GET_SCREEN: {
                screen_def_t* returned_screen = (screen_def_t*) config_buffer;
                if (requested_index < NSCREENS) {
                    *returned_screen = screens[requested_index];
                }
                break;
            }
            default:
                break;
        }
        config_buffer->crc32 = crc32((uint8_t*) config_buffer, CONFIG_SIZE - 4);
        return CONFIG_SIZE;
    }
    if (report_id == REPORT_ID_RUNTIME && reqlen >= RUNTIME_SIZE) {
        runtime_get_feature_t* runtime_buffer = (runtime_get_feature_t*) buffer;
        memset(runtime_buffer, 0, sizeof(runtime_get_feature_t));
        switch (last_runtime_command) {
            case RuntimeCommand::GET_MOUSE_CONFIG:
                fill_mouse_config((macos_mouse_config_t*) runtime_buffer);
                break;
            case RuntimeCommand::GET_DIAGNOSTICS:
                fill_runtime_diagnostics((runtime_diagnostics_t*) runtime_buffer);
                break;
            case RuntimeCommand::GET_STATUS:
            default:
                fill_runtime_status((runtime_status_t*) runtime_buffer);
                break;
        }
        runtime_buffer->crc32 = crc32((uint8_t*) runtime_buffer, RUNTIME_SIZE - 4);
        return RUNTIME_SIZE;
    }

    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (report_id == REPORT_ID_MULTIPLIER && bufsize >= 1) {
        memcpy(&resolution_multiplier, buffer, 1);
    }
    if (report_id == REPORT_ID_CONFIG && bufsize >= CONFIG_SIZE) {
        if (checksum_ok(buffer, CONFIG_SIZE) && version_ok(buffer)) {
            set_feature_t* config_buffer = (set_feature_t*) buffer;
            last_config_command = config_buffer->command;
            switch (config_buffer->command) {
                case ConfigCommand::RESET_INTO_BOOTSEL:
                    reset_usb_boot(0, 0);
                    break;
                case ConfigCommand::SET_CONFIG: {
                    set_config_t* config = (set_config_t*) ((set_feature_t*) buffer)->data;
                    if (config->partial_scroll_timeout == 0 ||
                        !constraint_mode_ok(config->constraint_mode) ||
                        config->offscreen_sensitivity == 0) {
                        break;
                    }
                    unmapped_passthrough = (config->flags & CONFIG_FLAG_UNMAPPED_PASSTHROUGH) != 0;
                    partial_scroll_timeout = config->partial_scroll_timeout;
                    set_interval_override_checked(config->interval_override);
                    constraint_mode = config->constraint_mode;
                    screens[-1].sensitivity = config->offscreen_sensitivity;
                    cursor_placement_interval_seconds = config->cursor_placement_interval_seconds;
                    persistent_mouse_config = config->mouse_config;
                    apply_mouse_config(&persistent_mouse_config);
                    fill_mouse_config(&persistent_mouse_config);
                    set_mapping_from_config();
                    break;
                }
                case ConfigCommand::CLEAR_MAPPING:
                    config_mappings.clear();
                    set_mapping_from_config();
                    break;
                case ConfigCommand::ADD_MAPPING: {
                    mapping_config_t* mapping_config = (mapping_config_t*) ((set_feature_t*) buffer)->data;
                    if (config_mappings.size() < max_persisted_mapping_count()) {
                        config_mappings.push_back(*mapping_config);
                        set_mapping_from_config();
                    }
                    break;
                }
                case ConfigCommand::GET_MAPPING:
                case ConfigCommand::GET_OUR_USAGES:
                case ConfigCommand::GET_THEIR_USAGES:
                case ConfigCommand::GET_SCREEN: {
                    get_indexed_t* get_indexed = (get_indexed_t*) ((set_feature_t*) buffer)->data;
                    requested_index = get_indexed->requested_index;
                    break;
                }
                case ConfigCommand::PERSIST_CONFIG:
                    need_to_persist_config = true;
                    break;
                case ConfigCommand::SUSPEND:
                    suspended = true;
                    break;
                case ConfigCommand::RESUME:
                    suspended = false;
                    // XXX clear input_state, sticky_state, accumulated?
                    break;
                case ConfigCommand::SET_SCREEN: {
                    set_screen_t* set_screen = (set_screen_t*) ((set_feature_t*) buffer)->data;
                    if (set_screen->index < NSCREENS && screen_ok(set_screen->screen)) {
                        screens[set_screen->index] = set_screen->screen;
                        screens_updated();
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    if (report_id == REPORT_ID_RUNTIME && bufsize >= RUNTIME_SIZE) {
        if (checksum_ok(buffer, RUNTIME_SIZE) && ((runtime_set_feature_t*) buffer)->version == CONFIG_VERSION) {
            runtime_set_feature_t* runtime_buffer = (runtime_set_feature_t*) buffer;
            last_runtime_command = runtime_buffer->command;
            switch (runtime_buffer->command) {
                case RuntimeCommand::SET_HOST_CURSOR: {
                    runtime_cursor_t cursor;
                    memcpy(&cursor, runtime_buffer->data, sizeof(cursor));
                    cursor.active_screen = 0;
                    set_cursor_from_host(cursor);
                    last_runtime_command = RuntimeCommand::GET_STATUS;
                    break;
                }
                case RuntimeCommand::SET_MOUSE_CONFIG: {
                    macos_mouse_config_t* mouse_config = (macos_mouse_config_t*) runtime_buffer->data;
                    apply_mouse_config(mouse_config);
                    last_runtime_command = RuntimeCommand::GET_STATUS;
                    break;
                }
                case RuntimeCommand::GET_MOUSE_CONFIG:
                case RuntimeCommand::GET_DIAGNOSTICS:
                case RuntimeCommand::GET_STATUS:
                case RuntimeCommand::NO_COMMAND:
                default:
                    break;
            }
        }
    }
}
