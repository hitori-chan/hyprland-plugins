# devtools

Dev tooling for exercising the plugins in the nested Hyprland
(`~/.local/share/hypr-nested/`). Not plugins (not in hyprpm.toml). Tracked:
`stress.sh`, `vptr.c`, `vkbd.c`, `input-capture.c`, `desktop_exec_test.cpp`,
`hyprosd_readback_test.cpp`, `fakes/wpctl`, the `Makefile`, and this README; the
binaries and the `*-proto.{c,h}` wayland-scanner glue are build artifacts
that `make` regenerates.

## desktop_exec_test.cpp

The standalone C++26 conformance matrix for the hyprbar launcher’s Desktop
Entry string/list decoding and `Exec=` parser. It does not start a compositor
or touch the session:

```
make test-desktop-exec
```

## hyprosd_readback_test.cpp

The standalone C++26 parser regression for the exact output emitted by
`wpctl get-volume`, including mute and comma-decimal forms:

```
make test-hyprosd
```

## stress.sh — the pre-deploy regression gate

Exact assertions over the nested harness + `vptr` + `vkbd` + `input-capture`: placement
memory, spawn/close storms, the notification cap, churn round-trips,
hostile state files, an input storm, a native `hyprland-input-capture-v1`
session, the shade's click/key verbs, acting-closes-the-shade, the bell click,
hyprosd's fake-`wpctl` process/readback path, the OSD card below an open shade,
a config reload, log hygiene. The fake changes no live PipeWire state.
Check #1 refuses to run when the installed headers'
`version.h` hash doesn't match the running binary. Run it before every
deploy; it must end `ALL CHECKS PASSED`.

Two traps it now guards against, both of which once made assertions pass
for the wrong reason: nothing may hard-code the nested monitor's size
(`retarget()` re-reads it, and `vptr` is given that extent), and nothing
may reach the nested daemon over the LOGIN session bus — `launch.sh` runs
the instance under its own `dbus-run-session`, and the host's own
hyprnotify answers to the same well-known name.

```
./stress.sh                        # gate the installed compositor
PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
  HYPR_DEPLOY_PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
  ./stress.sh ~/repo/Hyprland/build/Hyprland   # gate an uninstalled fork build
```

For an uninstalled fork, both variables must name the same disposable package
set. The first builds and hashes the plugins used by the nested compositor;
the second makes the deploy rehearsal check that fork instead of a stale
installed header cache. `stress.sh` repairs the generated `hyprland.pc`
prefix when `cmake --install --prefix` left it at `/usr/local`, and refuses to
run when the two paths differ. Never point either variable at the live
session's mapped plugin or compositor files.

Needs the nested harness at `~/.local/share/hypr-nested`.

## vptr — virtual-pointer injector

Injects real pointer input (`zwlr_virtual_pointer_v1`) into a compositor so
the plugins' `input.mouse.button` / `.move` listeners actually fire —
click-to-raise (hyprclick), snap-drag (hyprsnap), and every hyprbar click
(taglist / tasklist / tray / layoutbox). A `movecursor` dispatch only warps
the cursor; it does not emit the input events the plugins listen for.

```
make                       # needs wayland-scanner + wayland-client; HL=~/repo/Hyprland for the XML
WAYLAND_DISPLAY=<nested-wl> ./vptr <W> <H> <<'EOF'
move 640 400
press 272
release 272
EOF
```

- `argv`: monitor size in px (extent for absolute motion; default `1280 800`).
- stdin gesture script, one command per line — a whole press/move/release
  must be one invocation (the virtual pointer dies with the process):
  - `move X Y`      — absolute motion to a pixel
  - `rel DX DY`     — relative motion in compositor-space pixels
  - `press BTN` / `release BTN` — `BTN` = linux code: 272 left, 273 right, 274 middle
  - `scroll AXIS V` — `AXIS` 0 vertical / 1 horizontal
  - `sleep MS`      — pause (flushes first)

**SAFETY:** point it at the nested only. Run with
`WAYLAND_DISPLAY=$(cat ~/.local/share/hypr-nested/nested.wl)`; never leave it
unset or equal to the live socket, or it moves the real cursor and clicks the
real desktop. Guard `[ "$WL" != "$WAYLAND_DISPLAY" ]` before every run.

### nested config needed

The nested tiles by default and has no move bind, so the throwaway config
(`HYPR_CFG`) must add:

- a float-all rule (copy `rules.lua`'s `floating-only`) — hyprplace/hyprsnap
  only act on floats;
- for hyprsnap, a keyboard-free interactive move —
  `hl.bind("mouse:274", hl.dsp.window.drag(), { mouse = true })` plus
  `binds = { drag_threshold = 0 }`. Then a plain middle-drag starts the move
  and hyprsnap snaps.

## vkbd — virtual-keyboard injector

`vptr`'s twin for keys (`zwp_virtual_keyboard_v1`), so paths with no pointer
at all — the shade's nav set — ride the same emission a physical key does.
It compiles the default xkb keymap itself and hands it over before the first
key, as the protocol demands.

```
make                       # also needs xkbcommon
WAYLAND_DISPLAY=<nested-wl> ./vkbd <<'EOF'
tap down
tap delete
sleep 200
EOF
```

- stdin script, one command per line; the keyboard dies with the process,
  which is what keeps a stuck modifier from outliving a test:
  - `tap KEY` — press + release
  - `press KEY` / `release KEY`
  - `sleep MS`
- `KEY` = `esc`, `enter`, `space`, `up`, `down`, `delete`, `tab`, `a`, or a
  raw linux evdev code.
- Same **SAFETY** rule as `vptr`: point it at the nested socket only.

Still unbuilt: modifier chords (`vkbd` sends `modifiers` once, as zero), so
Super-gated paths (taglist `Mod+click`, hyprmax's immovable Super+drag
swallow) and the menubar's readline editing are still untested.

## input-capture - native capture receiver

`input-capture` binds the fork's `hyprland-input-capture-v1` manager, creates a
full-width top-edge barrier, consumes the passed EIS fd with libei, and waits
for a captured motion, left-button press/release, and key press/release. The
stress gate starts it only on the nested socket, then drives the barrier with
`vptr` and the key path with `vkbd`. The nested throwaway config grants the
receiver `input-capture` permission and the injector `keyboard` permission
without opening a dialog. The client is generated from the exact fork XML, so
this test is ABI/protocol-local to the target fork. The relative move crosses
past the barrier rather than ending on it; the fork's segment test treats an
endpoint as a boundary case. Starting `vkbd` after activation intentionally
replaces the compositor keymap and exercises EIS keyboard-device recovery.
The target fork keeps the active EIS emulation sequence across a recreated
device and a late capability bind.
