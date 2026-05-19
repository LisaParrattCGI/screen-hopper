# Screen Hopper Live Sync

Small macOS menu-bar utility that reports the local cursor position to the
Screen Hopper device and edits the persistent device configuration.

It sends cursor feature reports reactively when macOS reports mouse movement or
drag events. The reported position is local to the Mac host's composite desktop;
Screen Hopper stamps the source screen from the USB/forwarder path and
translates that position into the configured global screen layout. A slower
timer keeps configuration synced and provides a fallback
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
--report-rate N            fallback pointer report rate in Hz, default 0
--poll-interval N          seconds between fallback/config sync ticks, default 1
```

The debug window shows the latest host cursor report, the translated device
status, and edge-push state so border switching can be diagnosed directly.

Menu items:

- `Configure...` reads persistent configuration, validates edits, and writes it
  back only when Save is pressed. Discard closes the window without writing.
- `Launch at Login` uses macOS login items when running from the app bundle on
  macOS 13 or newer.
