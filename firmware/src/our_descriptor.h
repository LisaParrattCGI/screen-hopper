#ifndef _OUR_DESCRIPTOR_H_
#define _OUR_DESCRIPTOR_H_

#include <stdint.h>

#include "types.h"

#define RESOLUTION_MULTIPLIER 120

#define MAX_INPUT_REPORT_ID 3

extern const uint8_t our_report_descriptor[];
extern const uint32_t our_report_descriptor_length;

#endif
