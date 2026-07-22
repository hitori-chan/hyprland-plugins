# devtools

Dev tooling for exercising the plugins in the nested Hyprland
(`~/.local/share/hypr-nested/`). Not plugins (not in hyprpm.toml). Tracked:
`stress.sh`, `vptr.c`, the `Makefile`, and this README; the `vptr` binary
and the `vptr-proto.{c,h}` wayland-scanner glue are build artifacts that
`make` regenerates.

## stress.sh — the pre-deploy regression gate

Exact assertions over the nested harness + `vptr`: placement memory,
spawn/close storms, the notification cap, churn round-trips, hostile state
files, an input storm, log hygiene. Check #1 refuses to run when the
installed headers' `version.h` hash doesn't match the running binary. Run
it before every deploy; it must end `ALL CHECKS PASSED`.

```
./stress.sh                        # gate the installed compositor
PKG_CONFIG_PATH=$SCRATCH/share/pkgconfig \
  ./stress.sh ~/repo/Hyprland/build/Hyprland   # gate an uninstalled fork build
```

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

Super-gated paths (taglist `Mod+click`, hyprmax's immovable Super+drag
swallow) and keyboard paths (menubar) need a virtual **keyboard** — not built
yet.
