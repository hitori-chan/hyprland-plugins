# devtools

The Wayland fixtures and D-Bus fakes the nested gate drives
([testing](../awesome/docs/testing.md)). Binaries and generated protocol
glue are build artifacts, built from the fork's protocol XML
(xdg-shell from the system's wayland-protocols):

```sh
make -C devtools HL=/path/to/Hyprland
```

## Wayland fixtures

- `vptr WIDTH HEIGHT`: a virtual pointer reading `move`, `rel`,
  `press`, `release`, `scroll`, `sleep`. One process, one gesture.
- `vkbd`: a virtual keyboard with its own keymap, reading `tap`,
  `press`, `release`, `mods`, `sleep`. One process, one chord: its
  keyboard dies with it, so no stuck modifier outlives a test.
- `vlock`: an ext-session-lock-v1 client. `lock`/`unlock` on stdin
  (EOF unlocks); prints `locked`, `finished` (denied), `unlocked` and
  what its surfaces hear (`kenter`, `kleave`, `key CODE`, `penter`,
  `pleave`, `button CODE`). Not named after a real locker, so an idle
  daemon's `pidof <locker> || lock` never takes it for the user's.
- `input-capture WIDTH HEIGHT`: checks motion, button and key delivery
  through the fork's input-capture protocol and EIS.
- `cliphold DELAY_MS TEXT`: owns the clipboard and delays or holds a
  transfer (cancellation and teardown).
- `sharehold [-d] [-n FRAMES] [INTERVAL_MS]`: a wlr-screencopy share of
  the first output, printing READY at the first frame. `-d` copies with
  damage, as a screencast (a still screen keeps a frame waiting); `-n`
  stops after that many frames and stays connected, as a screenshot
  tool.
- `toplevel APP_ID`: activates the first toplevel with that app id over
  wlr-foreign-toplevel, as a taskbar does.
- `fixwin WIDTH HEIGHT [TITLE] [drag]`: a fixed-size toplevel (min ==
  max, the dialog/splash shape). With `drag` it is its own title bar: a
  press dragged 8 px asks `xdg_toplevel.move` with that press's serial;
  it prints `press SERIAL`, `move SERIAL`, `up`.
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
- `fsask APP_ID [maximized]`: a toplevel asking fullscreen (or
  maximized) before its initial commit, as a game's `--fs` does; it
  follows every configure and prints each as `W H STATE...` (`map` after
  its first buffer).
- `askwin APP_ID`: asks for xdg-activation on each `ask` line (with the
  seat's latest serial), printing `asked` once handled (`rejected` for
  an empty token).
- `fifowin fifo|recreate FRAMES` presents FRAMES SHM frames through
  fifo-v1 as Mesa's Vulkan WSI does (each update sets and waits on the
  barrier) and prints `frames elapsed_ms max_wait_ms presented discarded
  late`. `recreate` replaces the fifo object a third of the way in and
  lets the frames in flight present before the new one exists, as a
  swapchain recreate does; a wait over 2 s exits 1 (a held barrier).
- `gtkgrid.py LOG [wayland|x11] [WAYLAND-TRACE] [cursor|shapes]` is a
  real GTK3 client (PyGObject) as a render-vs-input oracle: a floating
  CSD window of solid color blocks and a menu button whose GtkMenu is an
  xdg_popup of colored items. It logs what GTK hit (`HIT <color>` on a
  press, `SCROLL <color> <direction>` per wheel detent; the blue block
  takes smooth scrolls instead, `SMOOTH blue <source> <dx> <dy>` per delta
  and `STOP blue <source>`, the source as GTK names its device; `PINCH
  <color> <phase> <fingers> <scale> <turn>` per touchpad pinch event, the
  turn since the last in degrees; `MENU <color>` on
  an activated item, `KEY <name> <mods>` per key press, its modifiers
  `ctrl+shift` or `-`). A release logs `UP <color> <x> <y>`: the block
  that had the press (GTK's implicit grab), where it went up relative to
  it. The yellow block is a drag-and-drop source (`DRAGBEGIN`,
  `DRAGFAILED <result>`, `DRAGEND`). `cursor` sets its own cursor image
  (a 16x16 rose square); `shapes` names `text` over yellow and hides the
  cursor over red.
  `findcolors.py PNG NAME=R,G,B...` finds where each color is drawn in a
  capture, so a click on what is drawn must name the same block.

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
