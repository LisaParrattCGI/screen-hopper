#!/usr/bin/env python3

import argparse
import binascii
import json
import struct

import hid

from hid_protocol import (
    CONFIG_VERSION,
    REPORT_ID_RUNTIME,
    RUNTIME_COMMAND_GET_STATUS as GET_STATUS,
    RUNTIME_COMMAND_SET_HOST_CURSOR as SET_HOST_CURSOR,
    RUNTIME_SIZE,
    SCREEN_COORD_SCALE,
    open_runtime_device,
    read_feature_payload,
)


def screen_coord(value):
    return round(float(value) * SCREEN_COORD_SCALE)


def from_screen_coord(value):
    return value / SCREEN_COORD_SCALE


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


def get_status(device):
    send_command(device, GET_STATUS)
    payload = read_feature(device)
    x, y, active_screen = struct.unpack_from("<qqb", payload)
    return {
        "cursor": {
            "x": from_screen_coord(x),
            "y": from_screen_coord(y),
            "active_screen": active_screen,
        }
    }


def main():
    parser = argparse.ArgumentParser(description="Read or tweak Screen Hopper runtime state.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("get", help="print the current runtime cursor")

    set_cursor = subparsers.add_parser("set-cursor", help="set the local cursor position observed by this host")
    set_cursor.add_argument("x", type=float)
    set_cursor.add_argument("y", type=float)

    args = parser.parse_args()

    device = open_runtime_device(hid)

    if args.command == "get":
        print(json.dumps(get_status(device), indent=2))
    elif args.command == "set-cursor":
        send_command(device, SET_HOST_CURSOR, struct.pack("<qq", screen_coord(args.x), screen_coord(args.y)))
        print(json.dumps(get_status(device), indent=2))


if __name__ == "__main__":
    main()
