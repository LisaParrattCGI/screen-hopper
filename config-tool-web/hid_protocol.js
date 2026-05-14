export const REPORT_ID_CONFIG = 100;
export const REPORT_ID_RUNTIME = 101;
export const CONFIG_USAGE_PAGE = 0xFF00;
export const CONFIG_USAGE = 0x20;
export const RUNTIME_USAGE_PAGE = 0xFF01;
export const RUNTIME_USAGE = 0x21;
export const UNMAPPED_PASSTHROUGH_FLAG = 0x01;
export const STICKY_FLAG = 0x01;
export const CONFIG_SIZE = 60;
export const RUNTIME_SIZE = 60;
export const CONFIG_VERSION = 7;
export const VENDOR_ID = 0xCAFE;
export const PRODUCT_ID = 0xBAF3;
export const DEFAULT_PARTIAL_SCROLL_TIMEOUT = 1000000;
export const DEFAULT_SCALING = 1000;
export const DEFAULT_SENSITIVITY = 1000;
export const MOUSE_CONFIG_SCALE = 65536;
export const SCREEN_COORD_SCALE = 1000;
export const SCREEN_COUNT = 2;

export const SET_CONFIG = 2;
export const GET_CONFIG = 3;
export const CLEAR_MAPPING = 4;
export const ADD_MAPPING = 5;
export const GET_MAPPING = 6;
export const PERSIST_CONFIG = 7;
export const GET_OUR_USAGES = 8;
export const GET_THEIR_USAGES = 9;
export const SUSPEND = 10;
export const RESUME = 11;
export const SET_SCREEN = 12;
export const GET_SCREEN = 13;

export const GET_RUNTIME_STATUS = 1;
export const SET_RUNTIME_HOST_CURSOR = 2;
export const SET_RUNTIME_MOUSE_CONFIG = 3;
export const GET_RUNTIME_MOUSE_CONFIG = 4;

export const DEFAULT_MOUSE_CONFIG = {
    'tracking_speed': 0.6875,
    'pointer_resolution': 400.0,
    'frame_rate': 67.0,
    'fixed_multiplier': 1.0,
    'placement_tolerance': 0.5,
};
