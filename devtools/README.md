# devtools

The Wayland fixtures and D-Bus fakes the nested gate drives
([testing](../docs/testing.md)). Sources are tracked; binaries and the
generated protocol glue are build artifacts.

Build the helpers from the fork's protocol XML (xdg-shell comes from the
system's wayland-protocols):

```sh
make -C devtools HL=/path/to/Hyprland
```

## Wayland fixtures

- `vptr WIDTH HEIGHT` reads virtual pointer commands: `move`, `rel`,
  `press`, `release`, `scroll`, and `sleep`. One process owns one
  gesture.
- `vkbd` reads `tap`, `press`, `release`, `mods`, and `sleep` and
  installs its own xkb keymap. One process owns one chord: its keyboard
  dies with it, so no stuck modifier outlives a test.
- `input-capture WIDTH HEIGHT` verifies motion, button, and key delivery
  through the fork's input-capture protocol and EIS.
- `cliphold DELAY_MS TEXT` owns the nested clipboard and delays or
  indefinitely holds a transfer to test cancellation and teardown.
- `sharehold [-d] [-n FRAMES] [INTERVAL_MS]` holds a screen share of the
  first output (a wlr-screencopy client copying frames until it is
  killed) and prints READY at the first copied frame: the notify
  battery's quiet-while-sharing checks. `-d` copies with damage, as a
  screencast does (a still screen keeps a frame waiting); `-n` stops
  after that many frames and stays connected, as a screenshot tool
  leaves its session.
- `toplevel APP_ID` activates the first toplevel with that app id over
  wlr-foreign-toplevel, as a taskbar or a window switcher picks a window.
- `fixwin WIDTH HEIGHT [TITLE]` maps a fixed-size xdg-toplevel (min ==
  max, the dialog/splash shape) to test placement of windows that refuse
  to resize.
- `splashwin [W H MARGIN ID late parented resz vismargin pinx parentonly
  pgeo follow unmaxwhenmaxed]` maps an updater splash's shape: a CSD
  toplevel whose committed buffer exceeds the declared geometry (a
  shadow margin) with the frame pinned. Each flag is a fixed positional
  slot (`-` skips one) selecting a variant: a per-axis pin, a resizable
  CSD window, a transient parent, a client that follows its configures,
  a client that unmaximizes itself (the CSD restore button). With
  `SPLASHWIN_POINTER_LOG=<path>` it appends `at X Y` (surface-local,
  margin included) on every pointer enter and motion: where a click
  lands in the client's own frame.
- `fsask APP_ID [maximized]` maps a toplevel born fullscreen, as a game
  or a video player's `--fs` does (or maximized, as an app restoring its
  last state): it asks before its initial commit, follows every
  configure, and prints each as `W H STATE...` (`map` after its first
  buffer) until killed.
- `askwin APP_ID` maps a window that asks to be activated over
  xdg-activation (as a browser's "switch to tab" does) on each `ask` line
  on stdin, with the latest serial the seat sent it, and prints `asked`
  once the compositor handled it (`rejected` for an empty token).
- `fifowin fifo|recreate FRAMES` presents FRAMES SHM frames through
  fifo-v1 as Mesa's Vulkan WSI does (each update sets and waits on the
  barrier) and prints `frames elapsed_ms max_wait_ms presented discarded
  late`. `recreate` replaces the fifo object a third of the way in and
  lets the frames in flight present before the new one exists, as a
  swapchain recreate does; a wait over 2 s exits 1 (a held barrier).
- `gtkgrid.py LOG [wayland|x11] [WAYLAND-TRACE]` is a real GTK3 client
  (PyGObject) as a render-vs-input oracle: a floating CSD window of solid
  color blocks and a menu button whose GtkMenu is an xdg_popup of colored
  items. It logs what GTK hit (`HIT <color>` on a press, `MENU <color>`
  on an activated item). `findcolors.py PNG NAME=R,G,B...` finds where
  each color is drawn in a capture, so a click on what is drawn must
  name the same block.

## D-Bus and process fakes

- `fake-sni` serves one StatusNotifierItem plus a dbusmenu (a magenta
  22x22 pixmap, a root layout with doubled and trailing separators, a
  sub layout with a trailing one) on `DBUS_SESSION_BUS_ADDRESS`: the
  shell battery points it at the nested's private session bus and checks
  the strip, the separator trim and the cascade against it.
  `kill -USR1` asks for attention.
- `fakes/wpctl` logs its arguments to `AW_WPCTL_LOG` and models the
  sink's level and the two nodes' mutes (`AW_WPCTL_LEVEL_FILE`,
  `AW_WPCTL_MUTES_FILE`): it takes only the plugin's relative steps and
  mute toggles, a step moves only a level the file keeps, and the next
  readback says what they wrote. It hangs
  (`AW_WPCTL_HANG_FILE`, the lifecycle battery) or floods its output
  (`AW_WPCTL_FLOOD_FILE`, the system battery) on demand.
- `fakes/canberra-gtk-play` exits at once, or with `AW_SOUND_HANG_FILE`
  present appends its pid to that file's `.pid` and hangs (the notify
  and lifecycle batteries).
- `fakes/fake-sysbus` serves the host's private system bus:
  `org.freedesktop.login1`, whose `SetBrightness` writes a fake
  backlight under `AW_BACKLIGHT_DIR` and logs to `AW_LOGIND_LOG`, and
  `org.bluez`, whose `EmitVolume` (at `/`, `org.awesome.Gate`) sends a
  headset transport's Volume change (the system battery's backlight and
  headset checks, never the host's devices).
