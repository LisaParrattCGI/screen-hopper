#ifndef _REMAPPER_H_
#define _REMAPPER_H_

#include <stdint.h>

#include "types.h"

void set_mapping_from_config();
void handle_received_report(const uint8_t* report, int len, uint16_t interface);
void clear_pointer_resolution_remainders(uint8_t dev_addr);

void extra_init();
bool read_report();

void interval_override_updated();
void screens_updated();
runtime_cursor_t get_runtime_cursor();
void get_runtime_placement_flags(uint8_t& placement_active, uint8_t& placement_anchor_pending);
void set_cursor_from_host(const runtime_cursor_t& cursor);

#endif
