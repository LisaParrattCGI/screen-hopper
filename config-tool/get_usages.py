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

GET_CONFIG = 3
GET_OUR_USAGES = 8
GET_THEIR_USAGES = 9


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
) = struct.unpack_from("<BBLLLLB", payload)

if version != CONFIG_VERSION:
    raise Exception("Incompatible version")

usages = {"our_usages": [], "their_usages": []}

for command, key, count in [
    [GET_OUR_USAGES, "our_usages", our_usage_count],
    [GET_THEIR_USAGES, "their_usages", their_usage_count],
]:
    for i in range(0, count, 3):
        send_command(device, command, struct.pack("<L", i))
        payload = read_feature(device)
        usages_rle = struct.unpack_from("<6L", payload)
        for u, l in zip(*(iter(usages_rle),) * 2):
            if u != 0:
                usages[key].extend("{0:#010x}".format(u + i) for i in range(l))

print(json.dumps(usages, indent=2))
