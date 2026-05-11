VENDOR_ID = 0xCAFE
PRODUCT_ID = 0xBAF3

CONFIG_VERSION = 6
CONFIG_SIZE = 60
RUNTIME_SIZE = 60

REPORT_ID_CONFIG = 100
REPORT_ID_RUNTIME = 101

CONFIG_USAGE_PAGE = 0xFF00
CONFIG_USAGE = 0x20
RUNTIME_USAGE_PAGE = 0xFF01
RUNTIME_USAGE = 0x21

UNMAPPED_PASSTHROUGH_FLAG = 0x01
STICKY_FLAG = 0x01

MOUSE_CONFIG_SCALE = 65536
SCREEN_COUNT = 2

CONFIG_COMMAND_SET_CONFIG = 2
CONFIG_COMMAND_GET_CONFIG = 3
CONFIG_COMMAND_CLEAR_MAPPING = 4
CONFIG_COMMAND_ADD_MAPPING = 5
CONFIG_COMMAND_GET_MAPPING = 6
CONFIG_COMMAND_PERSIST_CONFIG = 7
CONFIG_COMMAND_GET_OUR_USAGES = 8
CONFIG_COMMAND_GET_THEIR_USAGES = 9
CONFIG_COMMAND_SUSPEND = 10
CONFIG_COMMAND_RESUME = 11
CONFIG_COMMAND_SET_SCREEN = 12
CONFIG_COMMAND_GET_SCREEN = 13

RUNTIME_COMMAND_GET_STATUS = 1
RUNTIME_COMMAND_SET_HOST_CURSOR = 2
RUNTIME_COMMAND_SET_MOUSE_CONFIG = 3
RUNTIME_COMMAND_GET_MOUSE_CONFIG = 4

DEFAULT_MOUSE_CONFIG = {
    "tracking_speed": 0.6875,
    "pointer_resolution": 400.0,
    "frame_rate": 67.0,
    "fixed_multiplier": 1.0,
    "placement_tolerance": 0.5,
}


def read_feature_payload(device, report_id, report_size, report_name):
    data = bytes(device.get_feature_report(report_id, report_size + 1))
    if len(data) < report_size + 1:
        actual_size = max(0, len(data) - 1)
        raise RuntimeError(
            f"{report_name} feature report is {actual_size} bytes, expected {report_size}. "
            "This usually means the device is running older firmware or macOS is still using "
            "a cached HID report descriptor; flash the current firmware and unplug/replug the device."
        )
    if data[0] != report_id:
        raise RuntimeError(f"{report_name} feature report returned ID {data[0]}, expected {report_id}")
    return data[1 : report_size + 1]


def _open_path(hid_module, path):
    try:
        return hid_module.Device(path=path)
    except TypeError:
        device = hid_module.device()
        device.open_path(path)
        return device


def open_feature_device(hid_module, usage_page, usage):
    enumerate_fn = getattr(hid_module, "enumerate", None)
    if enumerate_fn is not None:
        for candidate in enumerate_fn(VENDOR_ID, PRODUCT_ID):
            if candidate.get("usage_page") == usage_page and candidate.get("usage") == usage:
                path = candidate.get("path")
                if path is not None:
                    return _open_path(hid_module, path)

    return hid_module.Device(VENDOR_ID, PRODUCT_ID)


def open_config_device(hid_module):
    return open_feature_device(hid_module, CONFIG_USAGE_PAGE, CONFIG_USAGE)


def open_runtime_device(hid_module):
    return open_feature_device(hid_module, RUNTIME_USAGE_PAGE, RUNTIME_USAGE)
