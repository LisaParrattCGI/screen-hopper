#!/usr/bin/env python3

import binascii
import json
import struct
import sys

import hid

from hid_protocol import (
    CONFIG_COMMAND_ADD_MAPPING as ADD_MAPPING,
    CONFIG_COMMAND_CLEAR_MAPPING as CLEAR_MAPPING,
    CONFIG_COMMAND_PERSIST_CONFIG as PERSIST_CONFIG,
    CONFIG_COMMAND_RESUME as RESUME,
    CONFIG_COMMAND_SET_CONFIG as SET_CONFIG,
    CONFIG_COMMAND_SET_SCREEN as SET_SCREEN,
    CONFIG_COMMAND_SUSPEND as SUSPEND,
    CONFIG_SIZE,
    CONFIG_VERSION,
    REPORT_ID_CONFIG,
    STICKY_FLAG,
    UNMAPPED_PASSTHROUGH_FLAG,
    open_config_device,
)


def feature_report(command, payload=b""):
    body = struct.pack("<BB", CONFIG_VERSION, command) + payload
    if len(body) > CONFIG_SIZE - 4:
        raise ValueError("feature payload is too large")
    body += bytes(CONFIG_SIZE - 4 - len(body))
    return bytes([REPORT_ID_CONFIG]) + body + struct.pack("<L", binascii.crc32(body))


def send_command(device, command, payload=b""):
    device.send_feature_report(feature_report(command, payload))


config = json.load(sys.stdin)

device = open_config_device(hid)

send_command(device, SUSPEND)

partial_scroll_timeout = config.get("partial_scroll_timeout", 1000000)
unmapped_passthrough = config.get("unmapped_passthrough", True)
interval_override = config.get("interval_override", 0)
constraint_mode = config.get("constraint_mode", 0)
offscreen_sensitivity = config.get("offscreen_sensitivity", 1000)

flags = UNMAPPED_PASSTHROUGH_FLAG if unmapped_passthrough else 0

send_command(
    device,
    SET_CONFIG,
    struct.pack(
        "<BLBBL",
        flags,
        partial_scroll_timeout,
        interval_override,
        constraint_mode,
        offscreen_sensitivity,
    ),
)

send_command(device, CLEAR_MAPPING)

for mapping in config.get("mappings", []):
    target_usage = int(mapping["target_usage"], 16)
    source_usage = int(mapping["source_usage"], 16)
    scaling = mapping.get("scaling", 1000)
    layer = mapping.get("layer", 0)
    flags = STICKY_FLAG if mapping.get("sticky", False) else 0
    send_command(
        device,
        ADD_MAPPING,
        struct.pack("<LLlBB", target_usage, source_usage, scaling, layer, flags),
    )

for i, screen in enumerate(config.get("screens", [])):
    send_command(
        device,
        SET_SCREEN,
        struct.pack(
            "<BLLLLL",
            i,
            screen["x"],
            screen["y"],
            screen["w"],
            screen["h"],
            screen.get("sensitivity", 1000),
        ),
    )

send_command(device, RESUME)
send_command(device, PERSIST_CONFIG)
