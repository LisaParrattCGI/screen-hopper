# Shared Clipboard Design

Shared clipboard should be opt-in and disabled by default. The app should never
start exporting clipboard contents merely because both hosts are running live
sync.

## Shape

- Add a menu item named `Shared Clipboard` with an off-by-default checkmark.
- When enabled, the app watches `NSPasteboard.general.changeCount`.
- Text-only support should come first. Images and rich content can follow after
  the transport is proven.
- Clipboard payloads should move over volatile runtime reports, not persistent
  configuration reports.
- Payloads need chunking, sequence numbers, and an explicit maximum size because
  HID feature reports are small.
- Screen Hopper should only relay clipboard chunks while both sides have enabled
  the feature in their app session.
- Clipboard state should not be written to flash.

## Safety

- Default off on every launch.
- No automatic paste, only clipboard replacement.
- Ignore clipboard updates that originated from the other host to avoid echo
  loops.
- Consider a visible menu-bar state when sharing is enabled.

## Transport

The current runtime report is sized for cursor/status diagnostics, not bulk
data. A clipboard implementation should add separate runtime commands for:

- announcing clipboard metadata
- sending a payload chunk
- acknowledging or rejecting a sequence
- clearing pending clipboard transfer state

The forwarder back-channel used for runtime cursor reports is the right
physical path for this later, but the protocol should be extended
deliberately rather than overloading cursor reports.
