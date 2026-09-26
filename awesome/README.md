# awesome

The Rust rewrite of the eight C++ plugins in this repo — one `cdylib`
(`awesome.so`) that the fork `dlopen()`s across the narrow `cabi` C ABI.
Nothing C++ (no types, exceptions, references, or templates) crosses the
boundary; the design and phase plan live in
[`docs/awesome-rust-plan.md`](../docs/awesome-rust-plan.md).

**Current state — Phase 1 (the probe + the mini-bar).** The Phase 0 ABI
probe (below) is complete, and Phase 1 adds the first real render: a
mini-bar that proves the whole cabi render pipeline end to end.

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

It loads **last** in `hyprpm.toml` and cancels no input, so it cannot reorder
the C++ plugins' input priority table.

## Layout

- `build.rs` — `bindgen` over the fork's `cabi` header (allow-listed to the
  `hl_*` surface; `::libc` for the C types).
- `src/ffi.rs` — the **only** `unsafe` in the crate. The `bindgen` bindings
  plus thin safe wrappers; every C callback is wrapped in `catch_unwind` so a
  panic never unwinds across the boundary.
- `src/probe.rs` — safe Phase 0 behavior (the event/config/job probe).
- `src/bar.rs` — safe Phase 1 mini-bar (the canvas render, the RGBA icon,
  the updating text clock, and the 1 s damage timer).

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

The nested gate loads the probe alongside the C++ plugins and runs a dedicated
`ffi` battery (probe present + metadata round-trip + 3 clean config reloads +
a check that the mini-bar paints the bottom strip), with the clean-teardown
core check in the `lifecycle` battery covering the exit path. See
`devtools/stress/ffi.sh`.
