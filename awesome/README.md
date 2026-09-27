# awesome

The Rust rewrite of the eight C++ plugins in this repo — one `cdylib`
(`awesome.so`) that the fork `dlopen()`s across the narrow `cabi` C ABI.
Nothing C++ (no types, exceptions, references, or templates) crosses the
boundary; the design and phase plan live in
[`docs/awesome-rust-plan.md`](../docs/awesome-rust-plan.md).

**Current state — Phases 0-3 (probe + mini-bar + the six policy/OSD
module ports).** The Phase 0 ABI probe (below) is complete, Phase 1 adds
the first real render (a mini-bar that proves the whole cabi render
pipeline end to end), Phase 2 ports the full C++ policy plugins (`hyprmax`,
`hyprplace`, `hyprclick`, `hyprpad`), and Phase 3 adds `hyprsnap` (drag
magnetism + aerosnap) and `hyprosd` (the volume/brightness OSD) on the new
bus-thread subsystem. Every port is cut over so the Rust port — not the C++
original — is the one the gate validates. The remaining ports are
`hyprnotify` (Phase 4) and `hyprbar` (Phase 5).

Phase 0 (the probe) proves the C boundary is clean:

- the fork's loader takes the C path (`hyprPluginInitC`/`hyprPluginExitC`)
  and fills the plugin metadata from C strings the probe writes through the
  out-params;
- events (TICK, KEY, MOUSE_BUTTON, WINDOW_ACTIVE/OPEN/DESTROY) arrive with
  the right payloads through one dispatcher;
- a config keyword (`plugin:awesome:probe_tick`) registers and reads back;
- the job model (one-shot `defer`, repeating `timer`, an fd `watch` on a bus
  pipe) arms, fires, and — on exit — is cancelled before the context is
  dropped, so no job can fire into an unmapped `.so`.

Phase 1 (the mini-bar) proves the render pipeline: the fork's `cabi` render
API (`hl_render_listen` + a canvas of rect/glass/border/texture draws +
`hl_text_texture`/`hl_texture_from_rgba` + `hl_damage`) is driven by a
trampoline `IPassElement` the fork adds at `RENDER_POST_WINDOWS` each frame.
The mini-bar paints a **bottom** strip (glass + 1px border + accent line),
an RGBA icon (CPU pixels → `createTexture`), and a text clock that updates
on a 1 s timer — all through the warm/draw gate (crash class 4: textures are
built outside a frame; `draw()` only paints cached textures). It sits at the
bottom so it never overlaps the C++ hyprbar (top) while both are loaded
(Phases 1-5); Phase 5 grows it into the real top bar and Phase 6 removes the
C++ bar.

Phase 2 (the `hyprmax` port) proves the **write-side** cabi API: the fork
now exposes `hl_window_set_geom`/`hl_window_set_fs_mode`/
`hl_window_set_toplevel_maximized`/`hl_monitor_workarea`, the window
min-max-size + client-size-grant accessors, `hl_window_id`, and
`hl_super_held`, and wires the window/workspace/monitor policy events into
`hl_subscribe`. `max.rs` is a port of the C++ `hyprmax`: per-window
(client-told) maximize, adopt the compositor-granted maximize, remember and
restore the windowed box per app (persisted TSV), reflow on a monitor
reserved-area or workspace/monitor change, swallow a Super+click on a
maximized window, and handle born-fullscreen. It registers `hyprmax.toggle`
through the fork's `hl_lua_register`. Because `ConfigManager` gives the
first registration of a Lua `namespace.name` the win, the gate **cuts over**:
the C++ `hyprmax` is removed from the harness `nested.lua` load list so the
Rust port owns `hyprmax.toggle`, and the `windows` battery's maximize checks
(toggle, reserved-area reflow, restore, 10× round-trip) now validate the
Rust port. The C++ `hyprmax` is still **built** (it stays in the build list)
— only its load is dropped — until Phase 6 deletes it.

`hyprplace` (spawn placement with geometry memory) is the second Phase 2
port: a new window is born at the remembered size for its class, and the
remembered spot lands when it's free — otherwise the least-overlap spot
(KWin's default). A fixed-size toplevel (min == max) keeps the compositor's
centered spot and never touches the class row; X11 and parent-anchored
windows keep their own spot while it's free; a maximized or workarea-filling
window consumes no free space. Placement is deferred out of the map emission
(queue + one-shot job); the close-box is remembered synchronously on
`window.close` (a strong ref — the fork now wires it, since `window.destroy`
may already be null). The port needed placement queries on the fork
(`hl_window_is_x11`/`has_parent`/`override_redirect`/`monitor`/
`border_size`/`grant_exempt`) and a window-address identity check — a
self-block (comparing handle pointers instead of the window address) made the
remembered spot "occupied" and sent every spawn to least-overlap.

`hyprclick` (click and focus-raise policy) is the third Phase 2 port: a plain
left click (or a Super+right grab) raises the clicked window, a fullscreen
"raise" tucks the floaters back behind it by clearing the allowed-over flag
(never `lower()`), and only the keyboard/dispatch/switch focus reasons raise
(a hover never does). `focus_prev_here` / `focus_next` / `focus_prev` walk
the workspace in ARRIVAL order (the z-order is useless under click-to-raise,
which rotates it). The corpse guard swallows the tail of a fast double-click
on a click-to-close surface (a window that died under the cursor is not
retargeted). In the dispatch it runs after `max` (a Super-grab max swallowed
is never a raise click). The port needed a focus *setter* with an explicit
reason (`hl_focus_window_set` — the reason is what picks the raise), a fresh
cursor hit test, the focus history, and the monitor's active workspace; and
the version out-param had to be NUL-terminated (the `env!` &str sits
mid-rodata, so an unterminated read swallowed the next literal and broke the
metadata round-trip).

`hyprpad` (the touchpad policy) is the fourth Phase 2 port: the touchpad
turns off while an external (USB/Bluetooth) mouse is present and back on
when it's unplugged, and the `pad` module's toggle flips it by hand.
Hotplug rides the compositor's own device signal — the fork fires
`HL_EV_POINTER_CHANGED` on a pointer add (the backend `newPointer`) or
remove (a per-pointer destroy listener, set up lazily and kept alive in a
global so the signal's weak ref expires safely) — and the handler only
(re)arms a settle timer that coalesces a plug's burst into one re-check.
The flip is `hl_run_lua("hl.device({…})")` — the code `hyprctl eval`
reaches, minus the fork + socket round-trip. The port needed a pointer
handle + queries (`hl_pointers` / `is_touchpad` / `is_virtual` /
`connected` / `bus_type` / `name` / `id`) and the Lua-eval passthrough;
the settle timer is a one-shot job re-armed by cancel+re-arm (extend the
timeout on each hotplug). Its feedback cards (the async D-Bus Notify
"enabled"/"disabled"/"not found" cards) land with the bus thread in
Phase 3.

Phase 3 adds the **bus thread** (`src/bus.rs`) and the last two small
modules. The bus thread is the plan's D-Bus line made concrete: zbus on a
tokio current_thread runtime living on its own std::thread — no bus call
runs on the compositor's event loop, and the bus thread never blocks the
compositor (each command is one bounded async step, the 5 s method timeout
bounds a wedged daemon). The event loop talks to it through a bounded
command channel (sends drop under overload, as the C++ bounded post
queues did) and a reply queue + non-blocking wake pipe (the persistent
`hl_watch_fd_persistent` keeps the read end armed across EAGAIN/EOF;
the queue is the source of truth, the pipe byte is only the nudge). No
reconnect — a bus death turns the cards off, the keys keep working (the
C++ links' behavior).

`hyprsnap` is the first Phase 3 port: drag **magnetism** (a floating
drag's edges snap to the workarea/screen edges within `snap_distance`,
and a resize snaps to neighboring windows on the same workspace) plus
**aerosnap** (a drag near a monitor edge arms a slot preview — left/right
columns, top/bottom strips, or a fullscreen — drawn in the render pass
and committed on the drag's end). The magnetism runs as a deferred job
(one per motion, coalesced); the aerosnap zone is armed/reset in the
mouse-move handler and committed from the drag-end path. The port needed
drag-state + target-geometry reads on the fork (`hl_drag_target` /
`hl_drag_mode` / `hl_drag_threshold_reached` / `hl_drag_dragging_tiled`,
`hl_window_target_position` / `hl_window_set_position_global` /
`hl_window_warp_position_size`), config reads (`hl_config_int` + the
`HL_CFG_COLOR` type), `hl_monitor_containing` (true containment, null in a
gap — the nearest `hl_monitor_at` would arm a zone the pointer isn't in),
and a second render listener (the snap preview coexists with the bar's
element).

`hyprosd` is the second Phase 3 port, now the `osd` module
(`hl.plugin.osd.*`): nothing is drawn — the value cards ride the bus
thread's `Notify` (ids 9992 brightness / 9993 volume / 9995 mic, replaced
in place). Brightness is fork-free (sysfs read, ±5% linear steps, floor 2
raw, written through logind `Session.SetBrightness` on the system bus;
the card waits for logind's ack and a 500 ms trust window keeps a fast
repeat from re-stepping off stale sysfs). Volume/mic go through `wpctl`
subprocesses sequenced on the event loop (set → pidfd → get → stdout pipe
→ parse → card; two short forks per keypress, render/input never wait on
any of it), with the readback as the authoritative state (a parse failure
emits no card) and every queue/chain/child bounded.

**Naming — no legacy.** The namespace is the MODULE, not the old plugin
name: Lua methods are `hl.plugin.<module>.<method>` (`max`, `click`,
`pad`, `snap`, `osd`, …) and config keys are `plugin:<module>:<key>`. The
user's live `hypr*` config is migrated old→new exactly once at the Phase
6 cutover (the one documented breaking change). It loads **last** in
`hyprpm.toml` and, while the C++ plugins are still loaded, defers to their
input (the bar eats its strip first); as a module is cut over, the Rust
port takes that module's input in place of the C++ original.

## Layout

- `build.rs` — `bindgen` over the fork's `cabi` header (allow-listed to the
  `hl_*` surface; `::libc` for the C types).
- `src/ffi.rs` — the **only** `unsafe` in the crate. The `bindgen` bindings
  plus thin safe wrappers; every C callback is wrapped in `catch_unwind` so a
  panic never unwinds across the boundary.
- `src/probe.rs` — safe Phase 0 behavior (the event/config/job probe).
- `src/bar.rs` — safe Phase 1 mini-bar (the canvas render, the RGBA icon,
  the updating text clock, and the 1 s damage timer).
- `src/max.rs` — safe Phase 2 `hyprmax` port (per-window maximize, adopt,
  remembered/restore windowed box, reserved-area + workspace reflow, the
  Super+click swallow, born-fullscreen). No `unsafe` — it calls only the
  safe `ffi` wrappers.
- `src/place.rs` — safe Phase 2 `hyprplace` port (spawn placement + geometry
  memory: remembered spot when free, else least-overlap; fixed-size / X11 /
  parent handling; border-aware on-screen clamp). No `unsafe`.
- `src/click.rs` — safe Phase 2 `hyprclick` port (click-to-raise, the
  fullscreen tuck, keyboard-focus-raises, arrival-order focus cycling, the
  corpse guard). No `unsafe`.
- `src/pad.rs` — safe Phase 2 `hyprpad` port (touchpad auto on/off on
  external-mouse presence, the settle-timer hotplug coalescing, the
  `pad.toggle` parity drain, the feedback cards on the bus thread).
  No `unsafe`.
- `src/snap.rs` — safe Phase 3 `hyprsnap` port (drag magnetism as a
  deferred job, the aerosnap edge-zone preview + commit). No `unsafe`.
- `src/bus.rs` — the one D-Bus thread (zbus on a tokio current_thread
  runtime on its own std::thread; bounded command channel, reply queue +
  wake pipe; no reconnect). The event-loop handle lives in a static
  (`bus::handle()` / `bus::stop()`) because the probe's State is leaked
  and cannot be mutated from teardown.
- `src/osd.rs` — safe Phase 3 `hyprosd` port (the volume/brightness
  chains: pidfd + stdout-pipe sequencing on the event loop, sysfs +
  logind brightness, the bounded orphan reaper). No `unsafe`.

## Build

```sh
make            # cargo build --release -> awesome.so
```

`HYPR_CABI_HEADER` (default: the fork source `src/plugins/cabi/cabi.h`)
selects the ABI header; the gate points it at the staged header matching the
gated binary. The runtime `cabiAbiVersion()` handshake is the real guard — a
stale header ejects cleanly rather than crashing.

The crate is `edition 2024` with `unsafe_code = "deny"` and
`unused = "deny"` at the crate root; `clippy` runs at `pedantic` with
`-D warnings`. All `unsafe` is confined to `src/ffi.rs`.

## Gate

The nested gate loads the probe alongside the still-unported C++ plugins
and runs the full battery; each ported module's own checks (maximize/reflow
and placement in `windows.sh`, click/focus in `focus.sh`, the wpctl process
path in `reply.sh`, the mini-bar in `ffi.sh`) validate the Rust port once
its C++ original is cut over (removed from the harness `nested.lua` load
list). The snap port — the C++ original never had a dedicated battery for
it — is validated by the full battery staying green across the drag and
workspace-switch paths. The clean-teardown core check in the `lifecycle`
battery covers the exit path, including the bus thread's sources-out
ordering.
