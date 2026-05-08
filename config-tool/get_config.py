#!/usr/bin/env python3

import hid
import binascii
import struct
import json

VENDOR_ID = 0xCAFE
PRODUCT_ID = 0xBAF3

CONFIG_VERSION = 6
CONFIG_SIZE = 64
REPORT_ID_CONFIG = 100
MOUSE_CONFIG_SCALE = 65536

GET_CONFIG = 3
GET_MAPPING = 6
GET_SCREEN = 13

UNMAPPED_PASSTHROUGH_FLAG = 0x01

NSCREENS = 2


def from_fixed16(value):
    return value / MOUSE_CONFIG_SCALE


def feature_report(command, payload=b""):
    body = struct.pack("<BB", CONFIG_VERSION, command) + payload
    if len(body) > CONFIG_SIZE - 4:
        raise ValueError("feature payload is too large")
    body += bytes(CONFIG_SIZE - 4 - len(body))
    return bytes([REPORT_ID_CONFIG]) + body + struct.pack("<L", binascii.crc32(body))


def send_command(device, command, payload=b""):
    device.send_feature_report(feature_report(command, payload))


def read_feature(device):
    data = bytes(device.get_feature_report(REPORT_ID_CONFIG, CONFIG_SIZE + 1))
    payload = data[1 : CONFIG_SIZE + 1]
    if binascii.crc32(payload[:-4]) != struct.unpack_from("<L", payload, CONFIG_SIZE - 4)[0]:
        raise Exception("CRC mismatch")
    return payload[:-4]


device = hid.Device(VENDOR_ID, PRODUCT_ID)

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
    cursor_placement_interval_seconds,
    tracking_speed,
    pointer_resolution,
    frame_rate,
    fixed_multiplier,
    placement_tolerance,
) = struct.unpack_from("<BBLLLLBBLL5L", payload)

if version != CONFIG_VERSION:
    raise Exception("Incompatible version")

config = {
    "version": version,
    "unmapped_passthrough": (flags & UNMAPPED_PASSTHROUGH_FLAG) != 0,
    "partial_scroll_timeout": partial_scroll_timeout,
    "interval_override": interval_override,
    "constraint_mode": constraint_mode,
    "offscreen_sensitivity": offscreen_sensitivity,
    "cursor_placement_interval_seconds": cursor_placement_interval_seconds,
    "mouse": {
        "tracking_speed": from_fixed16(tracking_speed),
        "pointer_resolution": from_fixed16(pointer_resolution),
        "frame_rate": from_fixed16(frame_rate),
        "fixed_multiplier": from_fixed16(fixed_multiplier),
        "placement_tolerance": from_fixed16(placement_tolerance),
    },
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

for i in range(NSCREENS):
    send_command(device, GET_SCREEN, struct.pack("<L", i))
    payload = read_feature(device)
    x, y, w, h, sensitivity = struct.unpack_from("<LLLLL", payload)
    config["screens"].append(
        {
            "x": x,
            "y": y,
            "w": w,
            "h": h,
            "sensitivity": sensitivity,
        }
    )


print(json.dumps(config, indent=2))
