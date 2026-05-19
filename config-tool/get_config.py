#!/usr/bin/env python3

import hid
import binascii
import struct
import json

from hid_protocol import (
    CONFIG_COMMAND_GET_CONFIG as GET_CONFIG,
    CONFIG_COMMAND_GET_MAPPING as GET_MAPPING,
    CONFIG_COMMAND_GET_SCREEN as GET_SCREEN,
    CONFIG_SIZE,
    CONFIG_VERSION,
    REPORT_ID_CONFIG,
    SCREEN_COORD_SCALE,
    SCREEN_COUNT,
    UNMAPPED_PASSTHROUGH_FLAG,
    open_config_device,
    read_feature_payload,
)


def from_screen_coord(value):
    return value / SCREEN_COORD_SCALE


def feature_report(command, payload=b""):
    body = struct.pack("<BB", CONFIG_VERSION, command) + payload
    if len(body) > CONFIG_SIZE - 4:
        raise ValueError("feature payload is too large")
    body += bytes(CONFIG_SIZE - 4 - len(body))
    return bytes([REPORT_ID_CONFIG]) + body + struct.pack("<L", binascii.crc32(body))


def send_command(device, command, payload=b""):
    device.send_feature_report(feature_report(command, payload))


def read_feature(device):
    payload = read_feature_payload(device, REPORT_ID_CONFIG, CONFIG_SIZE, "config")
    if binascii.crc32(payload[:-4]) != struct.unpack_from("<L", payload, CONFIG_SIZE - 4)[0]:
        raise Exception("CRC mismatch")
    return payload[:-4]


device = open_config_device(hid)

send_command(device, GET_CONFIG)
payload = read_feature(device)

(
    version,
    flags,
    partial_scroll_timeout,
    mapping_count,
    our_usage_count,
    their_usage_count,
    interval_override,
    constraint_mode,
    offscreen_sensitivity,
) = struct.unpack_from("<BBLLLLBBL", payload)

if version != CONFIG_VERSION:
    raise Exception("Incompatible version")

config = {
    "version": version,
    "unmapped_passthrough": (flags & UNMAPPED_PASSTHROUGH_FLAG) != 0,
    "partial_scroll_timeout": partial_scroll_timeout,
    "interval_override": interval_override,
    "constraint_mode": constraint_mode,
    "offscreen_sensitivity": offscreen_sensitivity,
    "screens": [],
    "mappings": [],
}

for i in range(mapping_count):
    send_command(device, GET_MAPPING, struct.pack("<L", i))
    payload = read_feature(device)
    target_usage, source_usage, scaling, layer, flags = struct.unpack_from("<LLlBB", payload)
    config["mappings"].append(
        {
            "target_usage": "{0:#010x}".format(target_usage),
            "source_usage": "{0:#010x}".format(source_usage),
            "scaling": scaling,
            "layer": layer,
            "sticky": (flags & 0x01) != 0,
        }
    )

for i in range(SCREEN_COUNT):
    send_command(device, GET_SCREEN, struct.pack("<L", i))
    payload = read_feature(device)
    x, y, w, h, sensitivity = struct.unpack_from("<LLLLL", payload)
    config["screens"].append(
        {
            "x": from_screen_coord(x),
            "y": from_screen_coord(y),
            "w": from_screen_coord(w),
            "h": from_screen_coord(h),
            "sensitivity": sensitivity,
        }
    )


print(json.dumps(config, indent=2))
