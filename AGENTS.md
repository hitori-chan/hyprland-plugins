# hyprland-plugins — working agreement for coding agents

One native C++26 Hyprland plugin inspired by AwesomeWM: the `awesome`
monolith (five modules: core, shell, windows, notify, system), its
nested-compositor gate in `awesome/gate/`, shared input/window fixtures
in `devtools/`, behavior docs in `awesome/README.md` with the design
contract in `docs/awesome-design.md`.

This file is the agreement between user and agent. It states principles
and hard lines, not project state. Broad user autonomy ("do your best")
never overrides the hard lines — they define what "do your best" may
touch. Where this file is silent, match the codebase's existing style
and use judgment; when scope is ambiguous, ask before expanding it.

Maintenance: add a rule only when an incident earns it, with the reason
attached; prune when the reason stops applying; keep the file lean — if
a section needs frequent updates, its content belongs in a source of
truth, not here.

## Where state lives

- Plugin version: `awesome/core/version.hpp` (must equal
  `hyprpm.toml`'s `[awesome]` version; the gate preflight enforces the
  lockstep).
- Fork state: `~/repo/Hyprland` — `git log` there is the truth for what
  the running binary is built from. Pre-bump states are tagged
  (`pre-bump-*`), so the fork is always restorable.
- Open issues and evidence limits: `TODO.md` (local, gitignored). Close
  an item by deleting its line; provenance for closed work is the
  commit history.
- Design contract: `docs/awesome-design.md` (tracked). Live-phase
  state and goal: `PLAN.md` (local, gitignored); the commit history is
  provenance.
- Behavior: `awesome/README.md` (the single behavior doc) and
  `docs/awesome-design.md` (the contract it implements).
- Gate results: the summary line of the run log
  (`== stress: ALL N CHECKS PASSED in Ns ==`), not shell exit codes.
- Live plugin load: probe with `hyprctl awesome <verb>` (e.g.
  `count`) — the plugin logs nothing on a successful load, so log
  greps can neither prove nor disprove a load (2026-10-03: a
  whole "plugin not loading" thread was a log-grep phantom).
- The nested's socket is NOT the nested process's `WAYLAND_DISPLAY`
  environ: that value is the display the nested CONNECTS to (the live
  socket), so `hyprctl`/`dispatch` aimed there hit the LIVE session.
  Read the provided socket from the harness (`$HARNESS/nested.sig` or
  the `SIG=` line of `$HARNESS/launch.log`), never from `/proc/<pid>/
  environ` (2026-10-04: probing "the nested" via its environ dispatched
  exec_cmd and window.close onto the live desktop, spawning fixture
  windows there and stealing live focus).

When in doubt, verify against these sources instead of memory or
conversation state. Never copy state into this file.

## Before editing

- Check `git status` and preserve existing worktree changes.
- Read the affected module's headers and `awesome/README.md`; for
  compositor integration read the exact fork sources in
  `~/repo/Hyprland`.
- Keep scope user-driven: no new feature, product-model change, or
  protocol-contract change just because an alternative seems preferable.
- User configs (`~/.config/hypr/...`) stay clean and minimal: no
  investigation notes, no comment dumps (the user ordered notes out of
  binds.lua — provenance belongs in `TODO.md`, not config). Config
  comments only for a non-obvious why.

## Environment and build

The plugin is ABI-locked to the exact `hitori-chan/Hyprland` fork
commit the compositor is built from; never assume upstream `main` is
compatible. A fork bump means re-checking the ABI surface (PluginAPI,
input capture/EIS, IME and native hit testing, renderer pass insertion,
monitor scanout hooks), rebuilding the plugin, and running the full
gate.

- Build with `make -C awesome`; its Makefile is standalone and owns the
  C++26 and ABI-sensitive flags. Do not add `-fvisibility=hidden`:
  `awesome/plugin.ver` localizes plugin symbols while Hyprland inline
  globals must stay unified for `dlopen`.
- The fork exposes a few members publicly that upstream keeps private
  (e.g. `CWaylandBackend::m_resource`, `CX11Backend::m_xwaylandSurface`)
  so plugins can read client state — read-only, documented in the fork
  headers.
- Dependencies come from distro packages (the `~/repo/<dep>` checkouts
  are exactly upstream, not fork dependencies); keep them at the fork's
  `flake.lock` pins; the user performs sudo installs.
- For an uninstalled fork, pass one package/header set through both
  `PKG_CONFIG_PATH` and `HYPR_DEPLOY_PKG_CONFIG_PATH` (same directory);
  never substitute a stale installed cache — watch stale-header
  resolution into `/usr/local/include` (dual-root redefinition errors)
  and the `-MMD` gap (`make -B` after a header install). After a fork
  commit, refresh the gate header set's `src/version.h` by COPYING
  `~/repo/Hyprland/src/version.h` (CMake-generated) verbatim — never a
  partial template substitution: the plugin's load-time hash guard
  compares a six-component hash (git hash + aquamarine/hyprutils/
  hyprgraphics/hyprcursor/hyprlang versions), and one unsubstituted
  `@VAR@` silently breaks the plugin load (2026-10-04: a partial
  refresh masked a full CSD battery failure as "plugin not loaded").
- Long builds and gate runs go in tmux. Every temporary artifact —
  logs, debug dumps, scratch files, probe dirs — goes under a
  dedicated `/tmp` subdirectory (e.g. `/tmp/hypr-gate/`); keep `~`
  free of temp stuff. `/tmp` is tmpfs, so nothing that must survive a
  reboot goes there. `cmd | tee` swallows the exit code — trust the
  log's summary line.
- The gate is `make -C awesome gate` (`awesome/gate/gate.sh`; ARGS
  passes through: the compositor bin and `-b`/`-k` battery selection).
  Fixtures build with `make -C devtools`.
  Battery tiers (2026-10-04 trim): default (no `-b`) = `quick` (the ~1-min
  smoke battery: load, strip, chip, CSD content-frame, maximize round-
  trip, one notification); `-b all` = shell windows state notify system
  pipeline lifecycle; `-b everything` = all + quick + focus (the X11-ping
  / activation battery). `state` and `focus` were split from `windows`
  because they are battle-tested and pay for relaunches — a geometry or
  CSD change runs `-b windows` (~1.5 min) alone, not the full gate.
  The harness resolves the LIVE instance from its control socket, never
  from the caller's `HYPRLAND_INSTANCE_SIGNATURE` (stale after a live
  relog: default-socket hyprctl fails rc=4 and the gate's exec_cmd dies —
  2026-10-04).
- The nested harness (scripts in `awesome/gate/`: launch/stop/shot/dev;
  runtime state in `$HYPR_HARNESS`, default `~/.local/share/hypr-nested`)
  parks a headless `nested-dev` output in the live session; if workspace
  switching misbehaves after gate runs, check `hyprctl monitors all -j`
  before suspecting the plugin. Teardown ALWAYS via the harness — it owns
  `output remove nested-dev`; raw-killing the nested PIDs leaves a phantom
  output that churns the renderer. launch.sh pins `nested-dev` to the
  live panel's mode/scale at creation (`hl.monitor` after `output create
  headless`): a divergent-mode output reconfigures the dmabuf feedback
  table on every join/leave, and one rapid divergent-mode
  remove+create pair killed the live compositor on this i915 box
  (2026-10-02). Never run launch/stop by hand while a gate is running
  — the gate's warmup relaunches a perturbed nested mid-battery, so a
  manual stop/launch corrupts the run and doubles the output churn.
  The harness NEVER focuses the live session (no live `hl.dsp.focus`
  dispatches, no cursor warps): the old parking/warmup focus dance
  stole the user's keyboard focus on every launch/warmup/stop
  (2026-10-02, user report) — frame-cycle warmup is capture-only now,
  and the harness's live-focus canary fails the gate if the live
  focus ever lands on `nested-dev`.

## Safety — hard lines

- Explicit user instructions are absolute and override the lines
  below: a directed action is executed, not re-litigated. The lines
  bind the agent's autonomous judgment — what "do your best" may
  touch — never a user-directed action.
- The agent never operates the live desktop: no `hyprpm update`/
  `enable`, plugin unload, live reload, or session exit. Deploy is the
  user's action (build + relog).
- Tests and gates never affect the live workspace and never take over
  the user's mouse or focus: harness scripts may create/move the
  off-screen `nested-dev` output and its window, but they must not
  focus live monitors/workspaces, warp the live cursor, or churn live
  state. Verified, not assumed: the harness canaries the live
  focused monitor+workspace across every launch/stop and fails the
  gate on a delta (2026-10-02: the parking focus dance violated this;
  the user's rule is absolute for all gate and probe scripts, and for
  any manual investigation launch).
- Never hot-swap a loaded plugin or overwrite a mapped `.so` in place
  (invariants 2 and 5). Let `hyprpm` own deployment, or build a
  complete artifact and rename it atomically.
- No destructive git operations (`reset --hard`, `checkout --`, branch
  deletion, force-push, rebase, history rewrite) without an explicit
  user request; preserve the user's worktree changes.
- Never rebuild or edit the plugin while a nested instance has it
  mapped.
- Keep environment identifiers (SSIDs, hostnames, MACs, user-specific
  socket paths) out of source, docs, tests, and commits.
- Do not claim a production regression or exploit without controlled
  evidence.
- At most one subagent at a time; keep the rest of the work on the main
  thread; verify subagent edits before reporting. Multi-agent
  orchestration only on explicit user request. A spawn that fails
  before its first tool use falls back to direct execution, not a
  retry.

## Crash-class invariants

Numbered because source comments cite these classes; never renumber
them, append new ones.

1. Never mutate the compositor's window list from a plugin.
2. Never unload or hot-swap a loaded plugin; `dlclose` during an
   sdbus-c++ exception can crash unwinding.
3. Never cancel key releases; reset every partial input state on
   session lock or relevant native capture-state change.
4. A texture cannot be painted in the frame that created it: use
   `awesome/core/canvas.hpp`'s warm/draw gate, create textures from the
   event loop, scissor paints to damage, and damage every
   visible-state transition (hover damages but does not rewarm).
5. Never overwrite a mapped `.so` in place.
6. Defer workspace and focus changes out of input emissions through
   `NAwesome::CHop`; teardown resets listeners before hops and makes
   newly armed hops no-ops.
7. Plugin input emissions run before compositor session-lock checks;
   every input listener checks `NAwesome::sessionLocked()` first and
   clears swallow masks, held counters, drag state, and armed zones
   there.
8. Compositor-side teardown: `CCompositor::cleanup()` destroys the
   window/workspace/monitor state before unloading plugins (the renderer
   holds smart refs into the .so, so the unload must stay later), so
   `~CWindow` & co. still emit bus events into our listeners over a
   half-dead state. All event-driven work no-ops from the moment
   `m_isShuttingDown` is set; the supervisor gates every state listener
   on `NAwesome::compositorShuttingDown()` (an event-driven bar warm over
   a released CSharedPointer owner SEGVs on nested teardown).

## Compositor integration

- Behavior follows AwesomeWM's vanilla semantics, source-verified
  (the contract lives in `docs/awesome-design.md`), tuned for
  Wayland/Hyprland where an X11/vanilla assumption is broken or
  unsafe: no per-app rules; every documented deviation says why.
- Use the fork's current public APIs and native renderer/input
  ownership; mutate private compositor state only when the exact target
  requires it, and document the dependency in code.
- Native layer surfaces, popups, IME surfaces, input-capture sessions,
  seat grabs, and implicit pointer grabs stay authoritative: recheck
  the native hit-test stack before intercepting a press, and pass
  through an active input-capture session.
- Keep scanout transitions behind the public full-render request hook;
  never edit direct-scanout state from a plugin.
- Render only after the warm/draw gate, use damage and scissor
  correctly, and keep stable geometry for the bar, menus, cards, and
  hit regions.
- Prefer universal behavior; a per-app rule is explicit configuration,
  not a substitute for correct Wayland protocol ordering.

## Code conventions

- Extend the `core/` helper that owns the concern instead of
  recreating it.
- Module symbols live in their module namespace
  (`NAwesome::Shell`, `NAwesome::Windows`, `NAwesome::Notify`,
  `NAwesome::System`); platform symbols in `NAwesome`; a required
  global `PHANDLE` is an ABI exception and must be documented.
- Keep D-Bus asynchronous and off render/input hot paths:
  `CBusLink::post()` for bus-originated work, `pollSoon()` for
  event-loop dispatch; never drain a connection inline.
- Bound every externally sized structure (queues, child-process work,
  action payloads, image data, decoded textures) and define behavior at
  the bound.
- Deferred replies, timers, subprocess callbacks, menu sessions, and
  replaced device/daemon state carry a generation or ownership check;
  teardown invalidates late callbacks before releasing their objects.
- Cross-module state travels through a documented core seam (the
  bell reads the notify model directly; the system module posts its
  feedback cards into it) — never through one module's private state
  from another.
- Keep input, render, and bus callbacks short; move blocking work off
  compositor dispatch where the target API permits.
- Comments only for hard constraints, non-obvious workarounds, magic
  values, cross-file contracts, and regression guards.

## Modules

The monolith is one plugin; the modules are an internal split with one
input pipeline (the supervisor dispatches button/move/axis/key to the
modules in a fixed order) and no inter-module bus.

- `core`: the platform — supervisor, input pipeline, hop queue, canvas
  (warm/draw gate), config schema, stores, jobs, fd.o bus client.
  Headless-tested (`make -C awesome test`).
- `shell`: the bar — workspaces, tasks (views), tray, bell, battery,
  clock, menubar launcher. Owns bar/menu input; the bell reads the
  notify model directly.
- `windows`: the window state machine and all placement —
  maximize/minimize/restore, click/focus policy, spawn placement, drag
  snap.
- `notify`: the `org.freedesktop.Notifications` daemon — cards,
  conversations, shade, popups, inline reply, sound.
- `system`: volume/mic (wpctl), brightness (logind), touchpad policy —
  every action's feedback is a notify card.

The input pipeline's module order is a behavior contract: the bar
claims its strip and menus before window policy, and notification
input beats the window below. It lives in the supervisor's dispatch
loop and must agree with `awesome/README.md`.

## Git and versions

- Published history is append-only: fast-forward pushes are normal;
  amend, rebase, retag, and force-push only on explicit user request
  (first verify the unpushed tail with `git merge-base --is-ancestor
  origin/main main`).
- One logical change per commit; imperative `scope: summary` subject of
  about 50 characters; scopes: `awesome:`, `devtools:`, `docs:`,
  `build:`, `all:`. No versions in subjects, no `Co-Authored-By`.
- Versions are MAJOR.MINOR.PATCH (redesign / feature / fix); keep
  `awesome/core/version.hpp` and `hyprpm.toml` in lockstep.
- Keep behavior docs current with code; update `TODO.md` when a change
  affects an open item.
- Never push a change that has not passed its build and the applicable
  nested checks.

## Definition of done

Before calling work complete:

1. The affected README/docs and `TODO.md` reflect the actual behavior.
2. The plugin builds against the exact target headers with C++26
   (`make -C awesome`) and `make -C awesome test` is green.
3. The relevant nested checks end with `ALL CHECKS PASSED`.
4. Teardown, reload, session-lock, native input precedence, damage,
   queue bounds, and late replies were considered for the change.
5. `git diff`/`status`, commits, and push result are reported
   accurately; no live plugin operation was performed by the agent.
