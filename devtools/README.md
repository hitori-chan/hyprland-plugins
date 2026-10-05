# devtools

Shared input and window fixtures for the plugin's nested gate
(`awesome/gate/`). Sources are tracked; binaries and generated protocol
glue are ignored build artifacts.

Build the helpers from the exact fork protocol XML:

```sh
make -C devtools HL=/path/to/Hyprland
```

- `vptr WIDTH HEIGHT` reads virtual pointer commands: `move`, `rel`,
  `press`, `release`, `scroll`, and `sleep`. One process owns one
  gesture.
- `vkbd` reads `tap`, `press`, `release`, `mods`, and `sleep`, installs
  its own xkb keymap, and exits without held state.
- `input-capture WIDTH HEIGHT` verifies motion, button, and key delivery
  through the fork's input-capture protocol and EIS.
- `cliphold DELAY_MS TEXT` owns the nested clipboard and delays or
  indefinitely holds a transfer to test cancellation and teardown.
- `fixwin WIDTH HEIGHT [TITLE]` maps a fixed-size xdg-toplevel (min ==
  max, the dialog/splash shape) to test placement of windows that refuse
  to resize.
- `splashwin W H MARGIN [ID] [late] [parented] [resz] [vismargin]
  [pinx] [parentonly] [pgeo] [follow] [unmaxwhenmaxed]` maps the
  Discord-updater-splash shape: a CSD toplevel whose committed buffer
  exceeds the declared geometry (a shadow margin) with the frame pinned —
  plus the per-axis-pin, resizable-CSD, transient-parent, configure-
  following and client-unmaximize (the CSD titlebar restore button)
  variants the windows battery drives against placement and maximize.
  With `SPLASHWIN_POINTER_LOG=<path>` it appends `at X Y` (surface-local,
  margin included) on every pointer enter and motion: where a click
  lands in the client's own frame.
- `gtkgrid.py LOG [wayland|x11] [WAYLAND-TRACE]` is a real GTK3 client
  (PyGObject) as a render-vs-input oracle: a floating CSD window of solid
  color blocks and a menu button whose GtkMenu is an xdg_popup of colored
  items; every press logs what GTK hit (`HIT <color>`, `MENU <color>`).
  `findcolors.py PNG NAME=R,G,B...` finds where each color is drawn in a
  capture, so a click on what is drawn must name the same block.

## D-Bus and process fakes

- `fake-sni` serves one StatusNotifierItem plus a dbusmenu (a magenta
  22x22 pixmap, a root layout with doubled and trailing separators, a
  sub layout with a trailing one) on the address given by
  `DBUS_SESSION_BUS_ADDRESS` — the tray battery points it at the nested
  instance's PRIVATE session bus (never the live one) and asserts the
  strip, the parse-time separator trim, and the cascade against it.
- `fakes/wpctl` logs its arguments, and hangs or floods on demand
  through `AW_WPCTL_*` marker files (the system battery's
  process-path checks).
- `fakes/canberra-gtk-play` records its invocation and hangs on demand
  through `AW_SOUND_HANG_FILE` (the notify battery's sound
  checks).

## Gate safety (still binds)

- The signature, socket, runtime directory, config, Wayland display, and
  D-Bus address must all belong to the nested compositor; injectors
  always receive the nested `WAYLAND_DISPLAY`.
- Never rebuild the plugin while the nested compositor maps its `.so`.
- Run the gate from a process rooted in the LIVE login session, not from
  a long-lived tmux server (the cgroup root is what poisons a
  session-outlived tmux; see `awesome/gate/preflight.sh`).
- A "dead" Wayland display is verified with `flock`, never by the lock
  file (empty flock files — `rm`-ing one breaks a live session).
- `kernel.core_pattern` is runtime-only: re-arm after any reboot or the
  clean-teardown check degrades to "unverified".
- The faked helpers never touch live devices.
