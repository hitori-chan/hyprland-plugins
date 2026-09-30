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
  [pinx] [parentonly] [pgeo]` maps the Discord-updater-splash shape: a
  CSD toplevel whose committed buffer exceeds the declared geometry (a
  shadow margin) with the frame pinned — plus the per-axis-pin,
  resizable-CSD, and transient-parent variants the windows battery
  drives against placement.
- `focustrap <map|attention|activate> [delay-s] [hold-s]` is the X11
  fixture: it maps a toplevel, waits, sends one unauthenticated EWMH
  ping — `_NET_ACTIVE_WINDOW` (activate) or
  `_NET_WM_STATE_DEMANDS_ATTENTION` (attention) — and holds; the focus
  checks assert the ping stays urgency-only and the keyboard focus never
  moves.

## D-Bus and process fakes

- `fake-sni` serves one StatusNotifierItem plus a dbusmenu (a magenta
  22x22 pixmap, a root layout with doubled and trailing separators, a
  sub layout with a trailing one) on the address given by
  `DBUS_SESSION_BUS_ADDRESS` — the tray battery points it at the nested
  instance's PRIVATE session bus (never the live one) and asserts the
  strip, the parse-time separator trim, and the cascade against it.
- `fakes/wpctl` logs its arguments, and hangs or floods on demand
  through `HYPROSD_WPCTL_*` marker files (the system battery's
  process-path checks).
- `fakes/canberra-gtk-play` records its invocation and hangs on demand
  through `HYPRNOTIFY_SOUND_HANG_FILE` (the notify battery's sound
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
