# PLAN — v0.56.2+ upstream audit: execution (2026-09-04)

Local working plan for the upstream audit (84 commits, v0.56.2-era base
`64962f89` → sync base `cb537a38`). Not tracked; provenance is the commit
history. Close items by moving them to Done.

## Findings

- **F1. initial-maximize was dropped.** Our `9216d4ed` (honor initial
  maximize at map) was lost when the upstream `af0d014c` views refactor
  was merged (the file moved; the hunk never re-landed). Upstream
  `d7a43351` additionally ignores pre-map maximize pongs at the protocol
  layer, so a verbatim re-apply would be dead code. Effect in the live
  session: session-restore maximizes (Firefox `sizemode=maximized`) no
  longer spawn maximized. hyprmax adopts compositor-granted maximize on
  sight, so this is user-visible.
- **F2. rounded-blur patch superseded.** `a86afb1d` is gone from the tree
  because upstream fixed the same bug itself (`cd83a26c` + the blur
  provider rewrite; `resolveBlurUV` is upstream's). Redundant — no action.
- **F3. Minimize.** X11 self-minimize is real (ICCCM/EWMH properties,
  `unminimize-on-activate` in `XWaylandManager`). Wayland has no
  compositor minimize: `CWaylandBackend::setMinimized` is a no-op. hyprbar
  implements awesome's `client.minimized` at plugin level
  (`hyprbar/tasklist.cpp`) and already routes Wayland clients' own
  `set_minimized` through `Tasklist::watchMinimize` (documented gap: X11
  clients unmap themselves, so no plugin state is needed). `eb97c46a`'s
  `window.minimize` Lua event has **no emitter yet** — dead plumbing until
  upstream ships true Wayland minimize. Nothing to wire now; revisit when
  it lands (tasklist state, restore path, binds).
- **F4. `config.unload` Lua event** — user-config feature; plugin teardown
  is `PLUGIN_EXIT`, not config unload. No code work.
- **F5. The other 12 fork patches survived the merge** (marker-verified:
  predictSize, workspace-backup, honest-maximized, restore-size,
  raise-on-press, EIS hardening x2, reserved-area signal, overlay
  activation, geometry align, plugin.* keys, hyprpm load order, LOG
  conversion).

## Work items

- **W1. fork: re-land initial-maximize (v2, post-refactor layout).**
  - `XDGShell.hpp`: `CXDGToplevelResource::m_wantsInitialMaximize`
    (non-volatile; distinct from the volatile `m_state` requests).
  - `XDGShell.cpp` `setSetMaximized`: record `true` on the ignored
    (pre-map) branch — the gate ignores exactly pre-map states, so an
    ignored maximize pong IS a pre-map request. `setUnsetMaximized`:
    `reset()` there (client's final pre-map intent).
  - `Window.cpp` map path (after the X11 `requestedClientFSMode` fallback,
    only when the pending client-FS slot is empty — fullscreen wins, as
    before): apply `FSMODE_MAXIMIZED`, consume the flag.
  - Echo safety: `FullscreenController::expectMaximizeEcho` covers the
    confirm pong (verify in W2 that the window STAYS maximized post-map).
  - Status: IN PROGRESS
- **W2. nested focused test.** Pre-commit `set_maximized` client (the
  upstream `xdg-initial-maximize` test client from `d7a43351`, or a small
  wlclient in `/tmp`): born maximized → stays maximized ≥ a few seconds →
  unmaximize round-trips to the remembered size; hyprmax adoption:
  toggling a born-maximized window via `hl.plugin.hyprmax.toggle` restores
  the remembered windowed box.
- **W3. rebuild fork → rebuild all plugins (fork headers changed) → full
  gate** (`ALL CHECKS PASSED`).
- **W4. ship.** Patch versions for the fork move (all plugins +
  `hyprpm.toml` lockstep), push fork + plugins, update `TODO.md`, user
  relogs.

## Done

(none yet)

## Out of scope (noted, no action)

- `b20621d4` pinned-floating position retention — behavior change only.
- `cursor:warp_on_monitor_change` — user config choice.
- `special_scale_factor` deprecation — unused by plugins and user config.
- True Wayland minimize — track upstream (see F3).
