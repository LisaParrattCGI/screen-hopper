#ifndef _OUR_DESCRIPTOR_H_
#define _OUR_DESCRIPTOR_H_

#include <stdint.h>

#include "types.h"

#define RESOLUTION_MULTIPLIER 120

#define REPORT_ID_MULTIPLIER 99
#define REPORT_ID_CONFIG 100
#define REPORT_ID_RUNTIME 101

#define MAX_INPUT_REPORT_ID 4

extern const uint8_t REPORT_ID_MOUSE;
extern const uint8_t REPORT_ID_MOUSE_RELATIVE;
extern const uint8_t REPORT_ID_KEYBOARD;
extern const uint8_t REPORT_ID_CONSUMER;

extern const uint8_t our_report_descriptor[];
extern const uint32_t our_report_descriptor_length;

#endif
