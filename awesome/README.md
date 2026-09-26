# awesome

The Rust rewrite of the eight C++ plugins in this repo — one `cdylib`
(`awesome.so`) that the fork `dlopen()`s across the narrow `cabi` C ABI.
Nothing C++ (no types, exceptions, references, or templates) crosses the
boundary; the design and phase plan live in
[`docs/awesome-rust-plan.md`](../docs/awesome-rust-plan.md).

**Current state — Phase 0 (the probe).** This build is the make-or-break
ABI probe, not the rewritten plugins. It proves the C boundary is clean:

- the fork's loader takes the C path (`hyprPluginInitC`/`hyprPluginExitC`)
  and fills the plugin metadata from C strings the probe writes through the
  out-params;
- events (TICK, KEY, MOUSE_BUTTON, WINDOW_ACTIVE/OPEN/DESTROY) arrive with
  the right payloads through one dispatcher;
- a config keyword (`plugin:awesome:probe_tick`) registers and reads back;
- the job model (one-shot `defer`, repeating `timer`, an fd `watch` on a bus
  pipe) arms, fires, and — on exit — is cancelled before the context is
  dropped, so no job can fire into an unmapped `.so`.

It loads **last** in `hyprpm.toml` and cancels no input, so it cannot reorder
the C++ plugins' input priority table.

## Layout

- `build.rs` — `bindgen` over the fork's `cabi` header (allow-listed to the
  `hl_*` surface; `::libc` for the C types).
- `src/ffi.rs` — the **only** `unsafe` in the crate. The `bindgen` bindings
  plus thin safe wrappers; every C callback is wrapped in `catch_unwind` so a
  panic never unwinds across the boundary.
- `src/probe.rs` — safe Phase 0 behavior (the event/config/job probe).

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
`ffi` battery (probe present + metadata round-trip + 3 clean config reloads),
with the clean-teardown core check in the `lifecycle` battery covering the
exit path. See `devtools/stress/ffi.sh`.
