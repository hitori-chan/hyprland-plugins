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
| `test-hyprsnap-geometry` | constrained snap geometry |
| `test-hyprmax-geometry` | constrained restore geometry |

## Nested Gate

`stress.sh` builds the eight plugins, launches the controlled compositor, and
tests load order, geometry policy, notifications, OSDs, reply/paste, DND,
management, snooze, fullscreen composition, input capture, reload, hostile
state, queue bounds, and teardown. Success requires its final
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
separated; from `windows notifications osd-reply policy lifecycle`; `all` is
the default). Canonical order is enforced regardless of user order, selecting
`lifecycle` auto-includes `osd-reply`, and preflight (builds, launch,
retarget) always runs. Without `lifecycle` selected, `stress.sh` itself prints
the final summary line.

The gate rejects mismatched package paths, target headers, and compositor
commits. `HYPR_STRESS_KEEP_STATE=1` retains screenshots and logs after a run.

The shell is split by ownership under `devtools/stress/`:

| Module | Responsibility |
|---|---|
| `harness.sh` | target validation, nested control, capture, shared assertions, stress config generation |
| `preflight.sh` | standalone tests, exact headers, builds, config, and launch |
| `windows.sh` | placement, persistence, maximize, snap, and window storms |
| `notify-lib.sh` | shared notification helpers: geometry constants, input gestures, Notify senders, panel measurement. Pure definitions, safe to source anywhere after `retarget` |
| `notifications.sh` | notification model, center, grouping, identity, and pixel checks |
| `osd-reply.sh` | OSD process/icon paths, fullscreen cards, reply, and clipboard |
| `policy.sh` | DND, management, snooze/undo, gestures, ranking, and admission |
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
config, waits out the compositor's 15s no-watchdog toast (it overlays the
panel column and poisons `panel_bottom`), and validates the target. The
probe's EXIT trap tears the nested and fixture state down; set
`HARNESS_CLEANED=1` before exiting to retain captures for inspection.

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
source devtools/stress/policy.sh              # the battery under test
```

Batteries are not pure definitions — sourcing one executes it. Batteries
that use helpers from an earlier battery (e.g. `lifecycle.sh` calls
`arm_reply`/`reply_keys` from `osd-reply.sh`) must be sourced after their
dependencies, exactly as `stress.sh` orders them. `policy.sh` and
`windows.sh` relaunch the nested themselves (persistence fixtures), so an
isolated run of either needs no prior state.

### Safety

- The signature, socket, runtime directory, config, Wayland display, and D-Bus
  address must all belong to the nested compositor.
- Injectors must always receive the nested `WAYLAND_DISPLAY`.
- Never rebuild a plugin while the nested compositor maps its `.so`.
- The generated config grants plugin, screencopy, keyboard, and input-capture
  permissions only inside the nested session.
- Faked `wpctl` and sound helpers never modify live devices.

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
