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
| `test-desktop-exec` | Desktop Entry decoding, indexing, and `Exec=` parsing |
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

The gate rejects mismatched package paths, target headers, and compositor
commits. `HYPR_STRESS_KEEP_STATE=1` retains screenshots and logs after a run.

The shell is split by ownership under `devtools/stress/`:

| Module | Responsibility |
|---|---|
| `harness.sh` | target validation, nested control, capture, and shared assertions |
| `preflight.sh` | standalone tests, exact headers, builds, config, and launch |
| `windows.sh` | placement, persistence, maximize, snap, and window storms |
| `notifications.sh` | notification model, center, grouping, identity, and pixel checks |
| `osd-reply.sh` | OSD process/icon paths, fullscreen cards, reply, and clipboard |
| `policy.sh` | DND, management, snooze/undo, gestures, ranking, and admission |
| `lifecycle.sh` | input capture, input storms, reload, logs, and teardown |

Modules execute in that order in one shell; shared state is intentional. Run
only the public `stress.sh` entrypoint.

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
