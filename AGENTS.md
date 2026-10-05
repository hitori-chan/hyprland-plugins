# hyprland-plugins — working agreement for coding agents

One native C++26 Hyprland plugin inspired by AwesomeWM: the `awesome`
monolith (modules core, shell, windows, notify, system), its gate in
`awesome/gate/`, test fixtures in `devtools/`, behavior in
`awesome/README.md`, the design contract in `docs/awesome-design.md`.

This file holds principles and hard lines, not project state. Optimize
for performance, efficiency, ergonomics, safety and stability; where this
file is silent, match the codebase and use judgment. Add a rule only when
an incident earns it (one line of why), prune it when the why is gone.

## Where state lives

- Plugin version: `awesome/core/version.hpp`, in lockstep with
  `hyprpm.toml`'s `[awesome]` version (the gate checks it).
- Fork state: `git log` in `~/repo/Hyprland` is the truth for what the
  compositor is built from; every rewrite of its history leaves a pushed
  backup tag (`pre-bump-*`, `pre-rewrite-*`).
- Open issues: `TODO.md` (local, gitignored); close an item by deleting
  it. Current phase and plan: `PLAN.md` (local). Provenance of closed work:
  the commit history.
- Gate results: the run log's summary line (`== stress: ALL N CHECKS
  PASSED in Ns ==`), not exit codes.
- Live plugin load: `hyprctl awesome count` (the plugin logs nothing on a
  successful load; log greps prove nothing).

## The fork

The plugin is ABI-locked to the exact `hitori-chan/Hyprland` commit the
compositor is built from.

- Keep the patch set minimal and clean: change the fork only when it is
  the true fix AND cleaner than plugin code. Plugin-only queries,
  compatibility shims and tooling conveniences live in the plugin or the
  harness. One logical change per fork commit, with a message that says
  why.
- A bump: rebase the series onto upstream, drop patches upstream made
  obsolete, rebuild, port the plugin to the new ABI, run the full gate
  (`-b all`). `src/version.h` is generated at CMake CONFIGURE time:
  reconfigure (`cmake -S . -B build`) after moving HEAD, or the binary and
  headers carry a stale hash the plugin's load guard rejects.
- Dependencies come from distro packages at the fork's `flake.lock` pins;
  the user performs sudo installs and deploys (install + relog, then
  `hyprpm update`).

## Build

- `make -C awesome` builds the release plugin (what hyprpm ships).
  `make -C awesome GATE=1` builds `awesome-gate.so`, the gate's variant.
- Test seams are compile-time only: `#ifdef AWESOME_GATE`, never an
  environment switch in the release binary.
- Do not add `-fvisibility=hidden`: `awesome/plugin.ver` localizes plugin
  symbols while Hyprland's inline globals must stay unified for `dlopen`.
- The build stamps its target (compiler, flags, the headers'
  `version.h`): switching header sets or installing a fork rebuilds
  everything by itself; no `make -B`.
- `make -C awesome test` runs the headless unit cases.

## The gate

`make -C awesome gate ARGS="-s ~/repo/Hyprland [-b quick|all|LIST]"`
stages the fork build's headers, builds both variants, and runs the
batteries against a nested Hyprland. Default tier `quick` (~1 min);
`-b windows` alone for a geometry/CSD change; `-b all` for a bump or a
release. Keep it lean: retire a check once its code is battle-tested,
gate new behavior with discriminating checks.

- Isolation is absolute: the nested runs inside a private headless labwc
  (`gate/host.sh`) with its own session bus, its own system bus (a fake
  logind and backlight) and scratch XDG dirs. The harness never sends the
  live session anything — no output, workspace, window, dispatch, focus,
  cursor, brightness or volume change — and reads it only for the
  isolation check, which fails the gate on any trace.
- Run gates and long builds in tmux; temporary artifacts go under
  `/tmp/hypr-gate/` (tmpfs); runtime state under `$HYPR_HARNESS`
  (default `~/.local/share/hypr-nested`). Keep `~` free of scratch files.
- Never edit gate scripts, rebuild the plugin, or run launch/stop by hand
  while a gate runs (bash reads battery scripts as it goes; the nested
  maps the `.so`).

## Safety — hard lines

- Explicit user instructions override the lines below; the lines bind
  the agent's own judgment.
- The live session is the user's: no `hyprpm update`/`enable`/`reload`,
  plugin unload, config reload or session exit by the agent, and no
  dispatches into it unless the user asked for live debugging.
- Never hot-swap a loaded plugin or overwrite a mapped `.so` in place
  (invariants 2 and 5).
- History: clean beats append-only. Amend, rebase and force-push
  (`--force-with-lease`) to keep history clean when the user allows it;
  push a backup tag first and verify the unpushed tail
  (`git merge-base --is-ancestor`). Preserve the user's worktree changes.
- Keep environment identifiers (SSIDs, hostnames, MACs, user-specific
  socket paths) out of source, docs, tests and commits.
- Claim a production regression or exploit only with controlled evidence.
- Subagents: read-only research and review may run in parallel; edits by
  one agent at a time; verify every subagent edit before reporting.

## Crash-class invariants

Numbered because source comments cite them; never renumber, append.

1. Never mutate the compositor's window list from a plugin.
2. Never unload or hot-swap a loaded plugin; `dlclose` during an
   sdbus-c++ exception can crash unwinding.
3. Never cancel key releases; reset every partial input state on session
   lock or a relevant native capture-state change.
4. A texture cannot be painted in the frame that created it: use
   `awesome/core/canvas.hpp`'s warm/draw gate, create textures from the
   event loop, scissor paints to damage, damage every visible-state
   transition (hover damages but does not rewarm).
5. Never overwrite a mapped `.so` in place.
6. Defer workspace and focus changes out of input and focus emissions
   through `NAwesome::CHop`; teardown resets listeners before hops and
   makes newly armed hops no-ops.
7. Plugin input emissions run before compositor session-lock checks:
   every input listener checks `NAwesome::sessionLocked()` first and
   clears swallow masks, held counters, drag state and armed zones there.
8. Shutdown is a half-dead state: `CCompositor::cleanup()` sets
   `m_isShuttingDown` before it unloads plugins (upstream now unloads them
   before clearing window/workspace/monitor state; an older order emitted
   `~CWindow` & co. into loaded listeners and SEGV'd on every nested
   teardown). All event-driven work no-ops once `m_isShuttingDown` is set;
   the supervisor gates every state listener on
   `NAwesome::compositorShuttingDown()`.

## Compositor integration

- Behavior follows AwesomeWM's vanilla semantics, source-verified
  (contract: `docs/awesome-design.md`), tuned where an X11/vanilla
  assumption is broken or unsafe on Wayland; every deviation says why. No
  per-app rules.
- Use the fork's public APIs and the native renderer/input ownership.
  Read private compositor state only where the target requires it, and
  document the dependency in code.
- Native layer surfaces, popups, IME surfaces, input-capture sessions,
  seat grabs and implicit pointer grabs stay authoritative: recheck the
  native hit-test stack before intercepting a press, and pass through an
  active input-capture session.
- Scanout: never edit direct-scanout or solitary state from the plugin;
  a layer that must paint over fullscreen says so (`ILayer::
  overFullscreen`, answered through `monitor.blockSolitary`).
- Render only after the warm/draw gate, damage and scissor correctly, and
  keep stable geometry for the bar, menus, cards and hit regions.

## Code conventions

- Extend the `core/` helper that owns a concern instead of recreating it
  (spawning: `core/proc.hpp` + `Jobs`; deferral: `CHop`; drawing: the
  canvas; persistence: `StateStore`).
- Module symbols live in their namespace (`NAwesome::Shell`, `::Windows`,
  `::Notify`, `::System`); platform symbols in `NAwesome`; a required
  global `PHANDLE` is a documented ABI exception.
- D-Bus stays asynchronous and off render/input hot paths
  (`CBusLink::post()`, `pollSoon()`); never drain a connection inline.
- Bound every externally sized structure (queues, children, payloads,
  images, textures, caches) and define behavior at the bound.
- Deferred replies, timers, subprocess callbacks, menu sessions and
  replaced device/daemon state carry a generation or ownership check;
  teardown invalidates late callbacks before releasing their objects.
- Cross-module state travels through a documented core seam, never
  through another module's private state.
- Keep input, render and bus callbacks short; per-frame and per-motion
  paths allocate nothing they can cache.
- Comments only for hard constraints, non-obvious workarounds, magic
  values, cross-file contracts and regression guards.

## Modules

One plugin, five internal modules, one input pipeline (the supervisor
dispatches button/move/axis/key to the modules in a fixed order), no
inter-module bus.

- `core`: supervisor, input pipeline, hops, canvas (warm/draw gate),
  config schema, stores, process runner and jobs, fd.o bus client.
- `shell`: the bar — workspaces, tasks, tray, bell, battery, clock,
  menubar launcher. Owns bar/menu input.
- `windows`: the window state machine and all placement —
  maximize/minimize/restore, click/focus policy, spawn placement, snap.
- `notify`: the `org.freedesktop.Notifications` daemon — cards,
  conversations, shade, popups, inline reply, sound.
- `system`: volume/mic (wpctl), brightness (logind), touchpad policy;
  every action's feedback is a notify card.

The pipeline order is a behavior contract (the bar claims its strip and
menus before window policy; notification input beats the window below);
it lives in the supervisor and must agree with `awesome/README.md`.

## Git and versions

- One logical change per commit; imperative `scope: summary` subject of
  about 50 characters; scopes `awesome:`, `devtools:`, `docs:`, `build:`,
  `all:`. No versions in subjects, no `Co-Authored-By`.
- Versions are MAJOR.MINOR.PATCH (redesign / feature / fix), lockstep in
  `version.hpp` and `hyprpm.toml`.
- Never push a change that has not passed its build and the applicable
  gate tier.

## Definition of done

1. README/design docs and `TODO.md` reflect the actual behavior.
2. Both variants build against the exact target headers and
   `make -C awesome test` is green.
3. The applicable gate tier ends with `ALL N CHECKS PASSED`, isolation
   included.
4. Teardown, reload, session lock, native input precedence, damage, queue
   bounds and late replies were considered for the change.
5. Commits and push results are reported accurately; no live-session
   operation was performed by the agent.
