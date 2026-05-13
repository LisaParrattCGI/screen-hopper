#ifndef _FORWARDER_CONTROL_H_
#define _FORWARDER_CONTROL_H_

#include <stdint.h>

const uint8_t FORWARDER_CONTROL_REPORT_ID = 0xFF;
const uint8_t FORWARDER_CONTROL_SET_ACTIVE = 1;

struct __attribute__((packed)) forwarder_control_t {
    uint8_t report_id;
    uint8_t command;
    uint8_t value;
};

#endif
