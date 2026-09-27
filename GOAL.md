# GOAL.md — finish the awesome rewrite, then harden it

Goal: fully implement `docs/awesome-rust-plan.md` so the single Rust
`awesome` plugin replaces all eight C++ plugins — then double-check,
clean up, and optimize with focus on performance, efficiency, ergonomics,
and safety. Do not stop until every item below is green. This file is the
checklist; the plan is the source of truth for behavior.

## Definition of done

### 1. Phase 3 complete — osd (+ the bus thread)
- The one bus thread (tokio single-thread runtime, zbus) exists in the
  crate; no bus call runs on the event-loop thread, ever.
- `osd` module ported: volume/brightness cards via the notify bus API
  (intra-process once notify is Rust; C++ API until then — unchanged
  behavior). Battery/volume events verified through the gate fakes.
- hyprpad D-Bus feedback cards land on the bus thread.
- Gate green; cutover done (NLOADED 4→3); pushed.

### 2. Phase 4 complete — notify
- `org.freedesktop.Notifications` served by the bus thread (zbus):
  Notify/Close/GetCapabilities/GetInformation + the action/reply flow.
- Model/policy: grouping, DND, history, urgent, timeouts — parity with
  the C++ model (the gate's notifications battery is the spec).
- Shade + banners render pixel-parity (the pixel-model port); input
  (hover, scroll, click, key reply) with the priority-table order
  (bar > notify > max > snap > click).
- hyprctl command + Lua methods (`hl.plugin.notify.*`,
  `plugin:notify:*` config) — no legacy names.
- Gate green; cutover done (NLOADED 3→2); pushed.

### 3. Phase 5 complete — bar
- Strip + widgets (clock, battery, net, task, taglist), menubar,
  bell, tray (SNI watcher) + dbusmenu, strip input — the top of the
  priority table.
- The bar→notify bridge (`org.hitori.hyprnotify`) is an intra-process
  call once both are Rust; until then unchanged.
- Gate green (tray battery, menubar pixel checks); cutover done
  (NLOADED 2→1); pushed.

### 4. Phase 6 — the cutover
- All eight C++ plugin sources deleted; `common/` C++ helpers deleted
  (what the Rust crate no longer uses); `NPLUGINS=1`.
- `hyprpm.toml` = one entry (`awesome`), version bumped once for the
  cutover.
- `docs/awesome.md`: priority table, per-module config reference, the
  exact old→new config migration list. Plugin READMEs current.
- AGENTS.md updated: load-order → priority table, ownership section
  rewritten for the single plugin.
- The user's live `hypr*` config is migrated old→new exactly once (the
  one documented breaking change) — the migration itself is the
  user's action; the exact list ships in `docs/awesome.md`.
- `org.hitori.hyprnotify` deleted (intra-process).
- Gate `ALL PASSED` on the single-plugin config; pushed.

### 5. Double-check
- Full gate battery green from a cold start:
  `== stress: ALL N CHECKS PASSED in Ns ==` (trust the summary line, not
  the exit code).
- `hq plugin list` shows exactly one plugin; the load order is moot
  (one plugin); the priority table owns dispatch.
- Teardown, reload, session-lock, native-input precedence, damage,
  queue bounds, and late replies were re-considered for the final state.
- READMEs, `docs/awesome.md`, `TODO.md` reflect the actual behavior.
- `git status` clean; every commit gate-green; nothing pushed that did
  not pass its build + nested checks.

### 6. Clean up
- No dead code, no unused cabi functions, no orphaned `common/` helpers,
  no duplicated helpers between the deleted C++ and the crate.
- No temp artifacts outside `/tmp/hypr-*/`; no environment identifiers
  (SSIDs, hostnames, MACs, user-specific sockets) in source, docs, tests,
  commits.
- BPROBE-style instrumentation, if any reappears, is reverted before
  commit.

### 7. Optimize — performance, efficiency, ergonomics, safety
- **Performance**: the render hot path allocates nothing per frame
  beyond the refcounted texture cache; damage is scissored and
  coalesced; input and bus callbacks stay short with blocking work off
  the event loop; the single bus thread never blocks the compositor.
- **Efficiency**: every externally sized structure (queues, action
  payloads, decoded textures, bus messages) is bounded with defined
  behavior at the bound; every deferred reply/timer/callback carries a
  generation or ownership check; teardown invalidates late callbacks.
- **Ergonomics**: `hl.plugin.<module>.<method>` and
  `plugin:<module>:<key>` everywhere; flat module files; clippy pedantic
  + `-D warnings` clean; `cargo fmt` clean; comments only for hard
  constraints, non-obvious workarounds, magic values, cross-file
  contracts, regression guards.
- **Safety**: all unsafe confined to `ffi.rs`; no C++ type, reference,
  exception, or template crosses the cabi; handles are atomic-refcounted
  with weak compositor refs (expired → `HL_E_NOT_FOUND`, never
  dangling); `hl_ctx` thread check on every entry; every `hl_*` wrapped
  in try/catch → `HL_E_FAILED`; session-lock + native input-capture
  guards on every input path; crash-class invariants 1–7 hold.

## Execution order (each lands gate-green, then pushed)
1. This file (GOAL.md).
2. Bus thread + osd + pad cards (Phase 3) → gate → cutover → push.
3. notify (Phase 4), staged: bus service + model → render (pixel
   parity) → input + reply + Lua → gate → cutover → push.
4. bar (Phase 5), staged: strip + widgets → tray/menubar → input →
   gate → cutover → push.
5. The cutover (Phase 6): delete C++, toml, docs, AGENTS.md → gate →
   push.
6. Final pass: double-check, clean up, optimize (item 5–7) → full gate
   from cold start → push.

## Rules (from the plan's hard lines)
- Naming: the module namespace, never the old plugin name.
- The cabi is the only boundary; extend it, don't route around it.
- D-Bus off the event loop (the one bus thread).
- The priority table replaces load order: bar > notify > max > snap >
  click; place/pad/osd take no input.
- A module's C++ original is dropped from the harness load list only in
  the commit whose gate battery passes.
- Fork builds: additive cabi API commits before the Rust port that
  depends on them; `GIT_COMMIT_HASH` exported before reconfigure; staged
  headers always match the gated binary.
