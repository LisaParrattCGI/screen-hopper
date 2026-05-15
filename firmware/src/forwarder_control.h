#ifndef _FORWARDER_CONTROL_H_
#define _FORWARDER_CONTROL_H_

#include <stdint.h>

#include "types.h"

const uint8_t FORWARDER_CONTROL_REPORT_ID = 0xFF;
const uint8_t FORWARDER_CONTROL_SET_ACTIVE = 1;
const uint8_t FORWARDER_CONTROL_SET_HOST_CURSOR = 2;
const uint8_t FORWARDER_CONTROL_SET_MOUSE_CONFIG = 3;

struct __attribute__((packed)) forwarder_control_t {
    uint8_t report_id;
    uint8_t command;
    uint8_t value;
};

struct __attribute__((packed)) forwarder_cursor_report_t {
    uint8_t report_id;
    uint8_t command;
    runtime_cursor_t cursor;
};

struct __attribute__((packed)) forwarder_mouse_config_report_t {
    uint8_t report_id;
    uint8_t command;
    macos_mouse_config_t mouse_config;
};

#endif
