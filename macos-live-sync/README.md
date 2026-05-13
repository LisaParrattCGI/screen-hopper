# Screen Hopper Live Sync

Small macOS menu-bar utility that keeps Screen Hopper's runtime mouse model in
sync with the Mac it is attached to.

It sends volatile runtime HID feature reports once per second, and it also has a
configuration window for editing the persistent device configuration through the
configuration feature report.

Build:

```sh
./build.sh
```

The build creates both a command-line binary and an app bundle:

```text
ScreenHopperLive
.build/ScreenHopperLive.app
```

Run:

```sh
./ScreenHopperLive
```

Or open `.build/ScreenHopperLive.app` from Finder.

Options:

```text
--active-screen N          active screen to send with cursor updates, default -1
--pointer-resolution N     fallback pointer resolution, default 400
--frame-rate N             acceleration frame rate, default 67
--fixed-multiplier N       acceleration fixed multiplier, default 1
--placement-tolerance N    cursor placement tolerance, default 0.5
--poll-interval N          seconds between live sync ticks, default 1
```

The tool reads `com.apple.mouse.scaling` from the global macOS preferences and
uses the first HID mouse `HIDPointerResolution` property it can find. If the
pointer resolution is not exposed by macOS, it uses the configured fallback.

Menu items:

- `Configure...` reads persistent configuration, validates edits, and writes it
  back only when Save is pressed. Discard closes the window without writing.
- `Launch at Login` uses macOS login items when running from the app bundle on
  macOS 13 or newer.
