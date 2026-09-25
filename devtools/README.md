# devtools

Standalone regressions, Wayland input fixtures, and the required exact-fork
nested-compositor gate. Sources are tracked; binaries and generated protocol
glue are ignored build artifacts.

## Standalone Tests

Run every pure regression or one named target:

```sh
make -C devtools test
make -C devtools test-pixel-model
```

| Target | Contract |
|---|---|
| `test-icon-resolver` | theme, symbolic, SVG, and identity lookup |
| `test-battery-state` | Pixel battery attribution, color, and width |
| `test-desktop-exec` | Desktop Entry decoding, event-loop indexing, cancellation, and `Exec=` parsing |
| `test-hyprosd` | strict `wpctl` readback parsing |
| `test-pixel-model` | grouping, expansion, conversations, avatars, and bounds |
| `test-persist` | bounded geometry-state admission |
| `test-fileindex` | `.desktop` indexing: symlink resolution, dangling/oversized/NUL admission |
| `test-hyprsnap-geometry` | constrained snap geometry |
| `test-hyprmax-geometry` | constrained restore geometry |

## Nested Gate

`stress.sh` builds the eight plugins, launches the controlled compositor, and
tests load order, geometry policy, notifications, OSDs, reply/paste, DND,
X11 focus steal, the tray menu, fullscreen composition, input capture,
reload, hostile state, queue bounds, and teardown. Success requires its final
`ALL CHECKS PASSED` line.

```sh
./devtools/stress.sh
```

For an uninstalled fork, use one disposable package set for both build and
deployment rehearsal:

```sh
PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
HYPR_DEPLOY_PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
./devtools/stress.sh /path/to/Hyprland/build/Hyprland
```

`-b LIST` runs only the named batteries and `-k LIST` skips them (comma
separated; from `windows notifications reply focus tray lifecycle`; `all`
is the default). Canonical order is enforced regardless of user order,
flags may appear before or after the compositor-bin positional, and
preflight (parallel builds, launch, retarget) always runs. Without
`lifecycle` selected, `stress.sh` itself prints the final summary.
The summary line is preceded by a per-battery `ok`/`fail` breakdown — a
battery that silently lost checks (a skipped block, a relaunch that no-ops)
shows up as a lower count instead of a quiet green. The boundary guard
checks are counted and annotated separately, and a battery that ran no
checks of its own fails the gate outright (a dead battery script can only
produce the guard).

Harness-level invariants, checked on top of the batteries:

- After every battery (lifecycle excepted, which tears the nested down
  itself) the nested must be client-free; a fixture window leaked past a
  battery boundary is named in the failure (the focus battery once left its
  foot under the tray menu column and poisoned the next battery's panel
  geometry).
- `capture_nested` validates that the capture is exactly the nested
  monitor's size; a drifted frame is dropped and retried, not fed to the
  pixel metrics.
- Every nested the gate kills must die clean: `kill_nested` checks the
  coredump record for the killed pid (a SEGV-class teardown death writes
  one, a clean exit does not), reports a 5s survivor as a SIGKILL hang
  rather than letting it pass, and degrades to an explicit "unverified"
  line when `coredumpctl` or the systemd-coredump `core_pattern` is
  absent. This is what the teardown SEGV class (2026-09-04..25, fork fix
  `377b812e`) should have caught: it passed every gate for three weeks
  because nothing looked at the corpse.

The gate rejects mismatched package paths, target headers, and compositor
commits. `HYPR_STRESS_KEEP_STATE=1` retains screenshots and logs after a run.

The shell is split by ownership under `devtools/stress/`:

| Module | Responsibility |
|---|---|
| `harness.sh` | target validation, nested control, capture, shared assertions, stress config generation |
| `preflight.sh` | standalone tests, exact headers, builds, config, and launch |
| `windows.sh` | placement, persistence, maximize, snap, and window storms |
| `notify-lib.sh` | shared notification helpers: geometry constants, input gestures, Notify senders, panel measurement. Pure definitions, safe to source anywhere after `retarget` |
| `notifications.sh` | notification model, center, grouping, identity, DND, ranking, admission, gestures, and pixel checks |
| `reply.sh` | hyprosd's wpctl process path, the pointer-only shade close (an explicit close re-pops the absorbed stack), inline reply, and the launcher clipboard |
| `focus.sh` | X11 EWMH pings (the GOG/Proton focus steal): urgency only, focus stays, click still focuses; closes its own foot before the geometry batteries |
| `tray.sh` | StatusNotifierItem lifecycle against the bar (the `fake-sni` fixture): strip icon, menu separator trim, submenu cascade, outside-click close |
| `lifecycle.sh` | input capture, input storms, reload, logs, and teardown |
| `probe-env.sh` | bootstrap for isolated one-off probes (see below) |

Modules execute in that order in one shell; shared state is intentional. Run
only the public `stress.sh` entrypoint.

### Isolated Probes

When debugging one battery section, do not re-run the whole gate and do not
eval line ranges out of the batteries — source the bootstrap and the helper
library:

```sh
source devtools/stress/probe-env.sh
launch_stress_nested || exit 1          # or: retarget (a stress nested is up)
source devtools/stress/notify-lib.sh
# ...drive hq/vp/vk/click/expect_panel against the nested...
```

`launch_stress_nested` kills any stale stress nested, regenerates the stress
config, validates the target, and waits until the panel column is clear
before the first measurement. The stress config suppresses the fork's 15s
no-watchdog toast (it used to hang in the panel column and poison
`panel_bottom`); the wait remains as the launch readiness check. The probe's
EXIT trap tears the nested and fixture state down; set `HARNESS_CLEANED=1`
before exiting to retain captures for inspection.

### Isolated Battery Runs

To re-verify a single battery after fixing it, source the battery file
itself instead of running the whole gate. Two variables the gate normally
provides must be set first, and batteries that use another battery's helpers
depend on source order:

```sh
source devtools/stress/probe-env.sh
launch_stress_nested || exit 1
CAPTURE_LOG="$HARNESS/input-capture.log"      # normally from stress.sh
LOG="$HARNESS/nested.log"                     # normally from preflight.sh
source devtools/stress/notify-lib.sh
source devtools/stress/notifications.sh       # the battery under test
```

Batteries are not pure definitions — sourcing one executes it. Source them
in the canonical order, exactly as `stress.sh` does; `windows.sh`
relaunches the nested itself (the persistence fixture), so an isolated run
of it needs no prior state.

### Safety

- The signature, socket, runtime directory, config, Wayland display, and D-Bus
  address must all belong to the nested compositor.
- Injectors must always receive the nested `WAYLAND_DISPLAY`.
- Never rebuild a plugin while the nested compositor maps its `.so`.
- The generated config grants plugin, screencopy, keyboard, and input-capture
  permissions only inside the nested session.
- Run the gate from a process rooted in the LIVE login session (e.g. a
  terminal window in it), not from a long-lived tmux server: a tmux server
  outlived by its session (its cgroup still `session-N.scope` for a logged-
  out session) poisons every nested it launches — the window parks
  correctly but the render cycle never starts (no frame callback from
  live), so every nested-side capture starves and no external kick
  (window move, VM focus, internal dispatch) revives it. Env, devices,
  rlimits, seccomp and caps are identical either way; the cgroup root is
  the difference, and migrating into the live session's cgroup from
  outside is denied. Symptoms: `retarget` reports
  "nested render cycle dead; relaunching nested" up to 3×, then aborts.
  After any live-session relog, also refresh the tmux server's stale
  `HYPRLAND_INSTANCE_SIGNATURE` (from a terminal inside the live session:
  `tmux set-environment -g HYPRLAND_INSTANCE_SIGNATURE
  "$HYPRLAND_INSTANCE_SIGNATURE"`) — live-side parking fails silently on
  a stale signature.
- Faked `wpctl` and sound helpers never modify live devices.
- A "dead" Wayland display is verified with `flock`, never by the lock
  file: the `wayland-*.lock` files are EMPTY flock files, so file presence
  and even pid-liveness checks on them are meaningless — a dead display
  looks exactly like a live one to them (the 2026-09-25 false alarm; and
  `rm`-ing one while the display is live is how you break a live session).
- `kernel.core_pattern` is runtime-only: a reboot resets it, and a
  faulting teardown then writes no record the gate can see. The
  clean-teardown check detects the un-armed pattern and reports
  "unverified" instead of a silent pass; re-arm after any reboot with
  `sudo sysctl -w "kernel.core_pattern=|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h"`

## Wayland Fixtures

Build the helpers from the exact fork protocol XML:

```sh
make -C devtools HL=/path/to/Hyprland
```

- `vptr WIDTH HEIGHT` reads virtual pointer commands: `move`, `rel`, `press`,
  `release`, `scroll`, and `sleep`. One process owns one gesture.
- `vkbd` reads `tap`, `press`, `release`, `mods`, and `sleep`, installs its own
  xkb keymap, and exits without held state.
- `input-capture WIDTH HEIGHT` verifies motion, button, and key delivery through
  the fork's input-capture protocol and EIS.
- `cliphold DELAY_MS TEXT` owns the nested clipboard and delays or indefinitely
  holds a transfer to test cancellation and teardown.
- `fixwin WIDTH HEIGHT [TITLE]` maps a fixed-size xdg-toplevel (min == max,
  the dialog/splash shape) to test placement of windows that refuse to resize.
- `splashwin W H MARGIN [ID] [late] [parented] [resz] [vismargin] [pinx] [parentonly] [pgeo]`
  maps the Discord-updater-splash shape: a CSD toplevel whose committed
  buffer exceeds the declared geometry (a shadow margin) with the frame
  pinned — plus the per-axis-pin, resizable-CSD, and transient-parent
  variants the windows battery drives against placement.
- `focustrap <map|attention|activate> [delay-s] [hold-s]` is the X11
  fixture: it maps a toplevel, waits, sends one unauthenticated EWMH ping —
  `_NET_ACTIVE_WINDOW` (activate) or `_NET_WM_STATE_DEMANDS_ATTENTION`
  (attention) — and holds; the focus battery asserts the ping stays
  urgency-only and the keyboard focus never moves.

### D-Bus Fixtures

- `fake-sni` serves one StatusNotifierItem plus a dbusmenu (a magenta 22x22
  pixmap, a root layout with doubled and trailing separators, a sub layout
  with a trailing one) on the address given by `DBUS_SESSION_BUS_ADDRESS` —
  the tray battery points it at the nested instance's PRIVATE session bus
  (never the live one) and asserts the strip, the parse-time separator
  trim, and the cascade against it.
