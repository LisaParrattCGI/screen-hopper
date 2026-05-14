#include <stdio.h>
#include <deque>
#include <limits>

#include "descriptor_parser.h"
#include "globals.h"
#include "quirks.h"

const uint8_t HID_INPUT = 0x80;
const uint8_t HID_OUTPUT = 0x90;
const uint8_t HID_FEATURE = 0xB0;
const uint8_t HID_COLLECTION = 0xA0;
const uint8_t HID_USAGE_PAGE = 0x04;
const uint8_t HID_REPORT_SIZE = 0x74;
const uint8_t HID_REPORT_ID = 0x84;
const uint8_t HID_REPORT_COUNT = 0x94;
const uint8_t HID_USAGE = 0x08;
const uint8_t HID_USAGE_MINIMUM = 0x18;
const uint8_t HID_USAGE_MAXIMUM = 0x28;
const uint8_t HID_LOGICAL_MINIMUM = 0x14;
const uint8_t HID_LOGICAL_MAXIMUM = 0x24;
const uint8_t HID_PHYSICAL_MINIMUM = 0x34;
const uint8_t HID_PHYSICAL_MAXIMUM = 0x44;
const uint8_t HID_UNIT_EXPONENT = 0x54;
const uint8_t HID_UNIT = 0x64;
const uint32_t MOUSE_X_USAGE = 0x00010030;
const uint32_t MOUSE_Y_USAGE = 0x00010031;
const uint32_t HID_UNIT_ENGLISH_LINEAR_INCH = 0x13;

int32_t sign_extend(uint32_t value, uint8_t item_size) {
    if (item_size == 0) {
        return 0;
    }
    uint8_t bits = item_size * 8;
    if (bits >= 32) {
        return (int32_t) value;
    }

    uint32_t sign_bit = (uint32_t) 1 << (bits - 1);
    if (value & sign_bit) {
        value |= std::numeric_limits<uint32_t>::max() << bits;
    }
    return (int32_t) value;
}

int8_t decode_unit_exponent(uint32_t value, uint8_t item_size) {
    if (item_size == 1 && (value & 0xF0) == 0) {
        uint8_t nibble = value & 0x0F;
        return (int8_t) ((nibble & 0x08) ? (nibble | 0xF0) : nibble);
    }
    return (int8_t) sign_extend(value, item_size);
}

uint32_t pow10_u32(uint8_t exponent) {
    uint32_t result = 1;
    while (exponent--) {
        if (result > std::numeric_limits<uint32_t>::max() / 10) {
            return 0;
        }
        result *= 10;
    }
    return result;
}

uint32_t pointer_resolution_for_usage(uint32_t usage,
                                      bool is_relative,
                                      int32_t logical_minimum,
                                      int32_t logical_maximum,
                                      int32_t physical_minimum,
                                      int32_t physical_maximum,
                                      int8_t unit_exponent,
                                      uint32_t unit) {
    if (!is_relative || (usage != MOUSE_X_USAGE && usage != MOUSE_Y_USAGE)) {
        return 0;
    }
    if (unit != HID_UNIT_ENGLISH_LINEAR_INCH ||
        logical_maximum <= logical_minimum ||
        physical_maximum <= physical_minimum) {
        return 0;
    }

    uint64_t logical_range = (uint64_t) ((int64_t) logical_maximum - logical_minimum);
    uint64_t physical_range = (uint64_t) ((int64_t) physical_maximum - physical_minimum);
    uint64_t numerator = logical_range * MOUSE_CONFIG_SCALE;
    uint64_t denominator = physical_range;

    if (unit_exponent < 0) {
        uint32_t multiplier = pow10_u32((uint8_t) -unit_exponent);
        if (multiplier == 0 || numerator > std::numeric_limits<uint64_t>::max() / multiplier) {
            return 0;
        }
        numerator *= multiplier;
    } else if (unit_exponent > 0) {
        uint32_t multiplier = pow10_u32((uint8_t) unit_exponent);
        if (multiplier == 0 || denominator > std::numeric_limits<uint64_t>::max() / multiplier) {
            return 0;
        }
        denominator *= multiplier;
    }

    if (denominator == 0) {
        return 0;
    }

    uint64_t resolution = (numerator + denominator / 2) / denominator;
    if (resolution > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    return (uint32_t) resolution;
}

void mark_usage(std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>>& usage_map, uint32_t usage, uint8_t report_id, uint16_t bitpos, uint8_t size, bool is_relative, int32_t logical_minimum, uint32_t pointer_resolution, bool is_array = false, uint32_t index = 0, uint32_t count = 0) {
    usage_map[report_id].try_emplace(usage,
                                     (usage_def_t) {
                                         .report_id = report_id,
                                         .size = size,
                                         .bitpos = bitpos,
                                         .is_relative = is_relative,
                                         .is_array = is_array,
                                         .logical_minimum = logical_minimum,
                                         .pointer_resolution = pointer_resolution,
                                         .index = index,
                                         .count = count,
                                     });
}

void assign_interface_index(uint16_t interface) {
    if (interface_index.count(interface)) {
        return;
    }

    uint8_t i = 0;
    while (i < 31 && ((1 << i) & interface_index_in_use)) {
        i++;
    }

    // if we have more than 32 interfaces, they end up sharing bit 31

    interface_index[interface] = i;
    interface_index_in_use |= 1 << i;
}

void parse_descriptor(uint16_t vendor_id, uint16_t product_id, const uint8_t* report_descriptor, int len, uint16_t interface) {
    mutex_enter_blocking(&their_usages_mutex);
    parse_descriptor(their_usages[interface], has_report_id_theirs[interface], report_descriptor, len);
    apply_quirks(vendor_id, product_id, their_usages[interface], report_descriptor, len);
    assign_interface_index(interface);
    mutex_exit(&their_usages_mutex);
    their_descriptor_updated = true;
}

std::unordered_map<uint8_t, uint16_t> parse_descriptor(std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>>& usage_map, bool& has_report_id, const uint8_t* report_descriptor, int len) {
    int idx = 0;

    uint8_t report_id = 0;
    std::unordered_map<uint8_t, uint16_t> bitpos;  // report_id -> bitpos
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint32_t usage_page = 0;
    std::deque<uint32_t> usages;
    uint32_t usage_minimum = 0;
    uint32_t usage_maximum = 0;
    int32_t logical_minimum = 0;
    int32_t logical_maximum = 0;
    int32_t physical_minimum = 0;
    int32_t physical_maximum = 0;
    int8_t unit_exponent = 0;
    uint32_t unit = 0;

    while (idx < len) {
        if (report_descriptor[idx] == 0 && idx == len - 1) {
            break;
        }

        uint8_t item = report_descriptor[idx] & 0xFC;
        uint8_t item_size = report_descriptor[idx] & 0x03;
        if (item_size == 3) {
            item_size = 4;
        }
        uint32_t value = 0;
        idx++;
        for (int i = 0; i < item_size; i++) {
            value |= (uint32_t) report_descriptor[idx++] << (i * 8);
        }

        switch (item) {
            case HID_INPUT: {
                printf("Input %0lx\n", value);

                bool relative = value & (1 << 2);
                if ((value & 0x03) == 0x02) {  // scalar
                    if (usage_minimum && usage_maximum) {
                        uint32_t usage = usage_minimum;
                        for (uint32_t i = 0; i < report_count; i++) {
                            mark_usage(usage_map, usage, report_id, bitpos[report_id], report_size, relative, logical_minimum,
                                       pointer_resolution_for_usage(usage, relative, logical_minimum, logical_maximum, physical_minimum, physical_maximum, unit_exponent, unit));
                            if (usage < usage_maximum) {
                                usage++;
                            }
                            bitpos[report_id] += report_size;
                        }
                    } else if (!usages.empty()) {
                        uint32_t usage = 0;
                        for (uint32_t i = 0; i < report_count; i++) {
                            if (!usages.empty()) {
                                usage = usages.front();
                                usages.pop_front();
                            }
                            mark_usage(usage_map, usage, report_id, bitpos[report_id], report_size, relative, logical_minimum,
                                       pointer_resolution_for_usage(usage, relative, logical_minimum, logical_maximum, physical_minimum, physical_maximum, unit_exponent, unit));
                            bitpos[report_id] += report_size;
                        }
                    } else {
                        bitpos[report_id] += report_size * report_count;
                    }
                } else if ((value & 0x03) == 0x00) {  // array
                    if (usage_minimum && usage_maximum) {
                        uint32_t usage = usage_minimum;
                        for (int index = logical_minimum; index <= logical_maximum; index++) {
                            mark_usage(usage_map, usage, report_id, bitpos[report_id], report_size, relative, logical_minimum, 0, true, index, report_count);
                            if (usage < usage_maximum) {
                                usage++;
                            }
                        }
                    } else if (!usages.empty()) {
                        uint32_t usage = 0;
                        for (int index = logical_minimum; index <= logical_maximum; index++) {
                            if (!usages.empty()) {
                                usage = usages.front();
                                usages.pop_front();
                            }
                            mark_usage(usage_map, usage, report_id, bitpos[report_id], report_size, relative, logical_minimum, 0, true, index, report_count);
                        }
                    }
                    bitpos[report_id] += report_size * report_count;
                } else {  // constant
                    bitpos[report_id] += report_size * report_count;
                }

                usages.clear();
                usage_minimum = 0;
                usage_maximum = 0;
                break;
            }
            case HID_COLLECTION:
            case HID_OUTPUT:
            case HID_FEATURE:
                usages.clear();
                usage_minimum = 0;
                usage_maximum = 0;
                break;
            case HID_USAGE_PAGE:
                printf("Usage page %0lx\n", value);
                usage_page = value;
                break;
            case HID_REPORT_SIZE:
                printf("Report size %0lx\n", value);
                report_size = value;
                break;
            case HID_REPORT_ID:
                printf("Report ID %0lx\n", value);
                report_id = value;
                has_report_id = true;
                break;
            case HID_REPORT_COUNT:
                printf("Report count %0lx\n", value);
                report_count = value;
                break;
            case HID_USAGE: {
                printf("Usage %0lx\n", value);
                uint32_t full_usage = item_size <= 2 ? usage_page << 16 | value : value;
                usages.push_back(full_usage);
                break;
            }
            case HID_USAGE_MINIMUM: {
                printf("Usage minimum %0lx\n", value);
                uint32_t full_usage = item_size <= 2 ? usage_page << 16 | value : value;
                usage_minimum = full_usage;
                break;
            }
            case HID_USAGE_MAXIMUM: {
                printf("Usage maximum %0lx\n", value);
                uint32_t full_usage = item_size <= 2 ? usage_page << 16 | value : value;
                usage_maximum = full_usage;
                break;
            }
            case HID_LOGICAL_MINIMUM:
                printf("Logical minimum %0lx\n", value);
                logical_minimum = sign_extend(value, item_size);
                break;
            case HID_LOGICAL_MAXIMUM:
                printf("Logical maximum %0lx\n", value);
                logical_maximum = value;
                break;
            case HID_PHYSICAL_MINIMUM:
                printf("Physical minimum %0lx\n", value);
                physical_minimum = sign_extend(value, item_size);
                break;
            case HID_PHYSICAL_MAXIMUM:
                printf("Physical maximum %0lx\n", value);
                physical_maximum = sign_extend(value, item_size);
                break;
            case HID_UNIT_EXPONENT:
                printf("Unit exponent %0lx\n", value);
                unit_exponent = decode_unit_exponent(value, item_size);
                break;
            case HID_UNIT:
                printf("Unit %0lx\n", value);
                unit = value;
                break;
        }
    }

    for (auto& [report_id_, position] : bitpos) {
        position /= 8;  // final bit position becomes report size in bytes
    }

    return bitpos;
}

void clear_descriptor_data(uint8_t dev_addr) {
    mutex_enter_blocking(&their_usages_mutex);
    for (auto it = their_usages.cbegin(); it != their_usages.cend();) {
        uint16_t dev_addr_interface = it->first;
        if (dev_addr_interface >> 8 == dev_addr) {
            has_report_id_theirs.erase(dev_addr_interface);

            uint8_t index = interface_index[dev_addr_interface];
            interface_index.erase(dev_addr_interface);
            interface_index_in_use &= ~(1 << index);

            it = their_usages.erase(it);
        } else {
            it++;
        }
    }
    mutex_exit(&their_usages_mutex);
    their_descriptor_updated = true;
}
