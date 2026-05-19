#!/usr/bin/env python3

import argparse
import binascii
import json
import struct
import sys

import hid

from hid_protocol import (
    CONFIG_VERSION,
    DEFAULT_MOUSE_CONFIG,
    MOUSE_CONFIG_SCALE,
    PRODUCT_ID,
    REPORT_ID_RUNTIME,
    RUNTIME_COMMAND_GET_MOUSE_CONFIG as GET_MOUSE_CONFIG,
    RUNTIME_COMMAND_GET_STATUS as GET_STATUS,
    RUNTIME_COMMAND_SET_HOST_CURSOR as SET_HOST_CURSOR,
    RUNTIME_COMMAND_SET_MOUSE_CONFIG as SET_MOUSE_CONFIG,
    RUNTIME_SIZE,
    SCREEN_COORD_SCALE,
    VENDOR_ID,
    open_runtime_device,
    read_feature_payload,
)


def fixed16(value):
    value = round(float(value) * MOUSE_CONFIG_SCALE)
    return max(0, min(0xFFFFFFFF, value))


def from_fixed16(value):
    return value / MOUSE_CONFIG_SCALE


def screen_coord(value):
    return round(float(value) * SCREEN_COORD_SCALE)


def from_screen_coord(value):
    return value / SCREEN_COORD_SCALE


def mouse_config_payload(config):
    mouse_config = {**DEFAULT_MOUSE_CONFIG, **(config or {})}
    return struct.pack(
        "<6L",
        fixed16(mouse_config["tracking_speed"]),
        fixed16(mouse_config["pointer_resolution"]),
        fixed16(mouse_config["frame_rate"]),
        fixed16(mouse_config["fixed_multiplier"]),
        fixed16(mouse_config["placement_tolerance"]),
        fixed16(mouse_config["report_rate"]),
    )


def feature_report(command, payload=b""):
    body = struct.pack("<BB", CONFIG_VERSION, command) + payload
    if len(body) > RUNTIME_SIZE - 4:
        raise ValueError("feature payload is too large")
    body += bytes(RUNTIME_SIZE - 4 - len(body))
    return bytes([REPORT_ID_RUNTIME]) + body + struct.pack("<L", binascii.crc32(body))


def send_command(device, command, payload=b""):
    device.send_feature_report(feature_report(command, payload))


def read_feature(device):
    payload = read_feature_payload(device, REPORT_ID_RUNTIME, RUNTIME_SIZE, "runtime")
    if binascii.crc32(payload[:-4]) != struct.unpack_from("<L", payload, RUNTIME_SIZE - 4)[0]:
        raise Exception("CRC mismatch")
    return payload[:-4]


def decode_mouse_config(values):
    return {
        "tracking_speed": from_fixed16(values[0]),
        "pointer_resolution": from_fixed16(values[1]),
        "frame_rate": from_fixed16(values[2]),
        "fixed_multiplier": from_fixed16(values[3]),
        "placement_tolerance": from_fixed16(values[4]),
        "report_rate": from_fixed16(values[5]),
    }


def get_status(device):
    send_command(device, GET_STATUS)
    payload = read_feature(device)
    x, y, active_screen, placement_active, placement_anchor_pending, *mouse_values = struct.unpack_from("<qqbBB6L", payload)
    return {
        "cursor": {
            "x": from_screen_coord(x),
            "y": from_screen_coord(y),
            "active_screen": active_screen,
            "placement_active": bool(placement_active),
            "placement_anchor_pending": bool(placement_anchor_pending),
        },
        "mouse": decode_mouse_config(mouse_values),
    }


def main():
    parser = argparse.ArgumentParser(description="Read or tweak Screen Hopper runtime state.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("get", help="print the current runtime cursor and mouse settings")

    set_cursor = subparsers.add_parser("set-cursor", help="set the cursor position observed by the host")
    set_cursor.add_argument("x", type=float)
    set_cursor.add_argument("y", type=float)
    set_cursor.add_argument("active_screen", type=int, nargs="?", default=-1)

    subparsers.add_parser("set-mouse", help="read mouse settings JSON from stdin and apply them without persisting")

    args = parser.parse_args()

    device = open_runtime_device(hid)

    if args.command == "get":
        print(json.dumps(get_status(device), indent=2))
    elif args.command == "set-cursor":
        send_command(device, SET_HOST_CURSOR, struct.pack("<qqb", screen_coord(args.x), screen_coord(args.y), args.active_screen))
        print(json.dumps(get_status(device), indent=2))
    elif args.command == "set-mouse":
        config = json.load(sys.stdin)
        mouse_config = config.get("mouse", config)
        send_command(device, SET_MOUSE_CONFIG, mouse_config_payload(mouse_config))
        print(json.dumps(get_status(device), indent=2))


if __name__ == "__main__":
    main()
