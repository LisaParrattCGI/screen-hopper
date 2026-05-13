# Screen Hopper HID Protocol

`screen_hopper_hid.json` is the canonical description of the vendor-defined HID
feature reports used by Screen Hopper host tools and firmware.

The language-specific mirrors are intentionally small and mechanical:

- `firmware/src/types.h`
- `config-tool/hid_protocol.py`
- `config-tool-web/hid_protocol.js`
- `macos-live-sync/HIDProtocol.swift`

When the report IDs, command IDs, sizes, flags, fixed-point scale, or payload
layouts change, update the JSON spec and the mirrors in the same change.
