# Screen Hopper Live Sync

Small macOS menu-bar utility that reports the local cursor position to the
Screen Hopper device and edits the persistent device configuration.

It sends cursor feature reports reactively when macOS reports mouse movement or
drag events. A slower timer keeps configuration synced and provides a fallback
cursor refresh. The app also has a configuration window for editing the
persistent device configuration through the configuration feature report.

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
--reported-screen N        diagnostic override for the reported screen, default -1
--active-screen N          legacy alias for --reported-screen
--pointer-resolution N     fallback pointer resolution, default 400
--frame-rate N             acceleration frame rate, default 67
--fixed-multiplier N       acceleration fixed multiplier, default 1
--placement-tolerance N    cursor placement tolerance, default 0.5
--report-rate N            fallback pointer report rate in Hz, default 0
--poll-interval N          seconds between fallback/config sync ticks, default 1
```

Normal use does not require a screen option. The firmware path identifies
whether reports came through `screenhopper_a` or the forwarder and stamps the
runtime cursor report with the appropriate Screen Hopper screen.

The mouse options are retained for protocol compatibility and diagnostics; the
firmware no longer uses them to predict the host cursor.

Menu items:

- `Configure...` reads persistent configuration, validates edits, and writes it
  back only when Save is pressed. Discard closes the window without writing.
- `Launch at Login` uses macOS login items when running from the app bundle on
  macOS 13 or newer.
