# devtools

Standalone regressions and the controlled nested-compositor gate. Sources are
tracked; binaries and generated Wayland protocol glue are build artifacts.

## Standalone Tests

| Target | Coverage |
|---|---|
| `test-icon-resolver` | freedesktop, symbolic, and Adwaita icon lookup |
| `test-battery-state` | Pixel battery state, color, precedence, and measurement |
| `test-desktop-exec` | Desktop Entry decoding and `Exec=` parsing |
| `test-hyprosd` | exact `wpctl get-volume` parsing |
| `test-persist` | bounded geometry-state admission |
| `test-hyprsnap-geometry` | constrained snap boxes |
| `test-hyprmax-geometry` | constrained maximize restore boxes |

Run one target or the full set:

```sh
make -C devtools test-battery-state
make -C devtools test-icon-resolver test-battery-state test-desktop-exec \
  test-hyprosd test-persist test-hyprsnap-geometry test-hyprmax-geometry
```

## Nested Gate

`stress.sh` is the required pre-deploy gate. It builds all eight plugins and
exercises the nested compositor at `~/.local/share/hypr-nested/`, including:

- plugin load order, exact version/header matching, and deploy rehearsal;
- placement, persistence, maximize, snap, close/spawn storms, and hostile data;
- banners, center, pointer management, replies, DND, snooze, policy, and OSDs;
- reply empty-submit ownership, OSD-safe Clear all, and semantic footer/snooze
  source checks;
- distinct brightness, volume, and touchpad OSD pixel crops;
- native input capture, virtual input storms, fullscreen, reload, log hygiene;
- bounded teardown with deliberately stuck helper processes.

The run is successful only when its final line is `ALL CHECKS PASSED`.

```sh
./devtools/stress.sh
```

For an uninstalled fork, install its build to a disposable prefix and pass the
same package directory through both variables:

```sh
PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
HYPR_DEPLOY_PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
./devtools/stress.sh /path/to/Hyprland/build/Hyprland
```

The gate rejects different package paths and repairs the generated
`hyprland.pc` prefix when `cmake --install --prefix` leaves `/usr/local` in the
file. The installed `version.h`, plugin compiler flags, and gated binary must
identify the same commit.

Safety rules:

- The socket, runtime directory, config, and D-Bus session must belong to the
  nested compositor.
- Never leave `WAYLAND_DISPLAY` unset for an injector and never point it at the
  live socket.
- Do not rebuild a plugin while the nested compositor maps its `.so`.
- The throwaway config grants screencopy, keyboard, and input-capture permission
  only inside the nested session. Fakes do not modify live PipeWire state.

## Input Helpers

Build the helper clients against the target fork protocol XML:

```sh
make -C devtools HL=/path/to/Hyprland
```

### vptr

`vptr` injects virtual pointer motion, buttons, and axes. One process owns one
gesture, so press/move/release must be sent in the same invocation.

```sh
WAYLAND_DISPLAY=$NESTED_WL ./devtools/vptr "$NESTED_WIDTH" "$NESTED_HEIGHT" <<'EOF'
move 640 400
press 272
release 272
EOF
```

Commands: `move X Y`, `rel DX DY`, `press BTN`, `release BTN`,
`scroll AXIS VALUE`, and `sleep MS`. Button codes 272/273/274 are left/right/
middle. The width and height are the current nested output extent, not constants
to copy into tests.

Manual snap testing also needs the nested float-all rule, a middle-button window
drag bind, and zero drag threshold. `stress.sh` creates its own throwaway config.

### vkbd

`vkbd` installs an xkb keymap, injects keys, and exits without leaving held
state behind.

```sh
WAYLAND_DISPLAY=$NESTED_WL ./devtools/vkbd <<'EOF'
tap a
sleep 200
EOF
```

Commands: `tap KEY`, `press KEY`, `release KEY`, and `sleep MS`. Named keys are
`esc`, `enter`, `space`, `up`, `down`, `delete`, `tab`, and `a`; raw evdev codes
are accepted. Modifier chords are not implemented.

### input-capture

`input-capture` binds the fork's `hyprland-input-capture-v1`, creates a top-edge
barrier, consumes the EIS fd with libei, and verifies captured motion, button,
and key delivery. Starting `vkbd` after activation intentionally recreates the
keyboard device and tests EIS recovery. The client must be generated from the
same fork XML as the compositor under test.
