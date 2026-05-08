# Screen Hopper Live Sync

Small macOS menu-bar utility that keeps Screen Hopper's runtime mouse model in
sync with the Mac it is attached to.

It sends volatile runtime HID feature reports only; it does not persist firmware
configuration.

Build:

```sh
./build.sh
```

Run:

```sh
./ScreenHopperLive
```

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

