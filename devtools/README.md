# devtools

The Wayland fixtures and D-Bus fakes the nested gate and vclick's check
drive ([testing](../awesome/docs/testing.md)). Binaries and protocol glue
are build artifacts, from the fork's protocol XML (xdg-shell from the
system's wayland-protocols):

```sh
make -C devtools HL=/path/to/Hyprland
```

## Wayland fixtures

- `vptr WIDTH HEIGHT`: a virtual pointer reading `move`, `rel`, `press`,
  `release`, `scroll` (a wheel), `fscroll`/`fstop` (a touchpad's travel
  and lift), `sleep`. One process, one gesture.
- `vkbd`: a virtual keyboard with its own keymap, reading `tap`, `press`,
  `release`, `mods`, `sleep`. One process, one chord: its keyboard dies
  with it, so no modifier outlives a test.
- `vlock`: an ext-session-lock-v1 client. `lock`/`unlock` on stdin (EOF
  unlocks); prints `locked`, `finished` (denied), `unlocked` and what its
  surfaces hear (`kenter`, `kleave`, `key CODE`, `penter`, `pleave`,
  `button CODE`). Not named after a real locker, so an idle daemon's
  `pidof <locker> || lock` never takes it for the user's.
- `input-capture WIDTH HEIGHT`: motion, button and key delivery through
  the fork's input-capture protocol and EIS.
- `cliphold DELAY_MS TEXT`: owns the clipboard and delays or holds a
  transfer.
- `sharehold [-d] [-n FRAMES] [INTERVAL_MS]`: a wlr-screencopy share of
  the first output, printing `READY` at the first frame. `-d` copies with
  damage, as a screencast; `-n` stops after that many frames and stays
  connected, as a screenshot tool.
- `toplevel APP_ID`: activates the first toplevel with that app id over
  wlr-foreign-toplevel, as a taskbar does.
- `fixwin WIDTH HEIGHT [TITLE] [drag|press|popup]`: a fixed-size toplevel
  (min == max). `drag`: its own title bar, a press dragged 8 px asks
  `xdg_toplevel.move` with that press's serial; `press`: asks at the
  press; prints `press SERIAL`, `move SERIAL`, `up`. `popup`: a 200x80
  grabless `xdg_popup` 10 px under the window, 20 px in; prints presses.
- `splashwin [W H MARGIN ID late parented resz vismargin pinx parentonly
  pgeo follow unmaxwhenmaxed]`: an updater splash's shape, a CSD toplevel
  whose buffer exceeds its declared geometry (a shadow margin). Each flag
  is a positional slot (`-` skips): per-axis pin, resizable, transient
  parent, following configures, unmaximizing itself.
  `SPLASHWIN_POINTER_LOG=<path>` appends `at X Y` (surface-local, margin
  included) on every enter and motion.
- `fsask APP_ID [maximized]`: asks fullscreen (or maximized) before its
  first commit, as a game's `--fs` does; prints each configure as `W H
  STATE...` (`map` after its first buffer).
- `askwin APP_ID`: asks xdg-activation on each `ask` line with the seat's
  latest serial; prints `asked` once handled (`rejected` for an empty
  token).
- `fifowin fifo|recreate FRAMES`: presents through fifo-v1 as Mesa's
  Vulkan WSI does; prints `frames elapsed_ms max_wait_ms presented
  discarded late`. `recreate` replaces the fifo object a third of the way
  in, as a swapchain recreate does; a wait over 2 s exits 1.
- `gtkgrid.py LOG [wayland|x11] [WAYLAND-TRACE] [cursor|shapes]`: a real
  GTK3 client as a render-vs-input oracle: a floating CSD window of solid
  colour blocks and a menu button whose GtkMenu is an `xdg_popup`. It
  logs:
  - `HIT <color>` on a press; `UP <color> <x> <y>` on the release (the
    pressed block, GTK's implicit grab);
  - `SCROLL <color> <direction>` per wheel detent; the blue block takes
    smooth scrolls instead: `SMOOTH blue <source> <dx> <dy>`, `STOP blue
    <source>`;
  - `PINCH <color> <phase> <fingers> <scale> <turn>` (turn in degrees
    since the last);
  - `MENU <color>` per activated item; a right press on green opens the
    items with the button down (`CONTEXT green`);
  - `KEY <name> <mods>` (`ctrl+shift` or `-`);
  - yellow is a drag source: `DRAGBEGIN`, `DRAGFAILED <result>`,
    `DRAGEND`.

  `cursor` sets a 16x16 rose cursor image; `shapes` names `text` over
  yellow and hides the cursor over red.
- `findcolors.py PNG NAME=R,G,B...`: where each colour is drawn in a
  capture, so a click on what is drawn must name the same block.

## D-Bus and process fakes

- `fake-sni`: one StatusNotifierItem and a dbusmenu (a magenta 22x22
  pixmap; root and sub layouts with doubled and trailing separators) on
  `DBUS_SESSION_BUS_ADDRESS`; a bus name as its argument makes another
  item. `kill -USR1` asks for attention.
- `fakes/wpctl`: logs its arguments to `AW_WPCTL_LOG`, models the sink's
  level and both mutes (`AW_WPCTL_LEVEL_FILE`, `AW_WPCTL_MUTES_FILE`; only
  the plugin's relative steps and toggles), and on demand hangs
  (`AW_WPCTL_HANG_FILE`) or floods its output (`AW_WPCTL_FLOOD_FILE`).
- `fakes/canberra-gtk-play`: exits at once, or with `AW_SOUND_HANG_FILE`
  present appends its pid to that file's `.pid` and hangs.
- `fakes/fake-sysbus`: the private system bus. `org.freedesktop.login1`'s
  `SetBrightness` writes a fake backlight under `AW_BACKLIGHT_DIR` and
  logs to `AW_LOGIND_LOG`; `org.bluez`'s `EmitVolume` (at `/`,
  `org.awesome.Gate`) sends a headset transport's Volume change.
