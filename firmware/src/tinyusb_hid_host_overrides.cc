#include <string.h>

#include <tusb.h>

#include "class/hid/hid_host.h"
#include "host/usbh_pvt.h"
#include "interval_override.h"

namespace {

bool hid_override_init() {
    return true;
}

bool hid_override_deinit() {
    return true;
}

uint8_t rounded_interval(uint8_t descriptor_interval) {
    uint8_t interval = 0;
    for (uint8_t bit_idx = 0; bit_idx < 6; bit_idx++) {
        uint8_t candidate = 1 << bit_idx;
        if (candidate <= descriptor_interval) {
            interval = candidate;
        }
    }
    return interval;
}

uint8_t effective_hid_interval(uint8_t dev_addr, uint8_t itf_num, tusb_desc_endpoint_t const* ep_desc) {
    (void) dev_addr;
    (void) itf_num;

    if (ep_desc->bInterval == 0) {
        return 0;
    }

    uint8_t local_interval_override = interval_override;
    return local_interval_override ? local_interval_override : rounded_interval(ep_desc->bInterval);
}

bool patch_hid_endpoint_intervals(uint8_t dev_addr, uint8_t itf_num, uint8_t* descriptor, uint16_t len) {
    uint8_t* pos = descriptor;
    uint8_t* const end = descriptor + len;

    while (pos < end) {
        if (pos + 2 > end || pos[0] == 0 || pos + pos[0] > end) {
            return false;
        }

        if (pos[1] == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t* ep_desc = (tusb_desc_endpoint_t*) pos;
            if (ep_desc->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                ep_desc->bInterval = effective_hid_interval(dev_addr, itf_num, ep_desc);
            }
        }

        pos += pos[0];
    }

    return pos == end;
}

bool hid_override_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const* itf_desc, uint16_t max_len) {
    if (itf_desc->bInterfaceClass != TUSB_CLASS_HID) {
        return false;
    }

    if (max_len > CFG_TUH_ENUMERATION_BUFSIZE) {
        return hidh_open(rhport, dev_addr, itf_desc, max_len);
    }

    uint8_t descriptor_copy[CFG_TUH_ENUMERATION_BUFSIZE];
    memcpy(descriptor_copy, itf_desc, max_len);

    if (!patch_hid_endpoint_intervals(dev_addr, itf_desc->bInterfaceNumber, descriptor_copy, max_len)) {
        return false;
    }

    return hidh_open(rhport, dev_addr, (tusb_desc_interface_t const*) descriptor_copy, max_len);
}

usbh_class_driver_t const hid_override_driver[] = {
    {
        .name = "HID interval override",
        .init = hid_override_init,
        .deinit = hid_override_deinit,
        .open = hid_override_open,
        .set_config = hidh_set_config,
        .xfer_cb = hidh_xfer_cb,
        .close = hidh_close,
    },
};

}  // namespace

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return hid_override_driver;
}
