# hyprland-plugins — working agreement for coding agents

Two native C++26 Hyprland plugins, sharing no code or state:

- `awesome/`: an AwesomeWM-style monolith (modules core, shell, windows,
  notify, system). Design contract `awesome/docs/design.md`, behavior per
  module in `awesome/docs/`, testing `awesome/docs/testing.md`, the gate
  `awesome/gate/`.
- `vclick/`: clicks, scrolls, drags or pinches inside a window without the
  user's pointer, at the user's learned pace. Units in `vclick/vclick.hpp`,
  behavior in `vclick/docs/`, nested check `vclick/check.sh`.

Test fixtures live in `devtools/`. Each plugin has its own `README.md` and
`docs/`; the root `README.md` only lists and installs them.

This file holds principles and hard lines, not project state. Optimize for
performance, efficiency, ergonomics, safety and stability; where it is
silent, match the codebase. Add a rule only when an incident earns it (one
line of why); prune it when the why is gone.

## Where state lives

- Versions: `awesome/core/version.hpp` and `VClick::VERSION`
  (`vclick/vclick.hpp`), each in lockstep with its `hyprpm.toml` section.
- The fork: `git log` in `~/repo/Hyprland`; every history rewrite leaves a
  pushed backup tag (`pre-bump-*`, `pre-rewrite-*`).
- Open issues: `TODO.md` (local; delete an item to close it). A phase's
  plan: `PLAN.md` (local, never tracked), deleted once the phase is done.
  Closed work: the commit history.
- Gate results: the summary line (`== stress: ALL N CHECKS PASSED in Ns
  ==`), not exit codes.
- Live plugin load: `hyprctl awesome count` (a good load logs nothing).

## The fork

The plugins are ABI-locked to the exact `hitori-chan/Hyprland` commit the
compositor is built from.

- Change the fork only when it is the true fix and cleaner than plugin
  code; queries, shims and tooling live in the plugin or the harness. One
  logical change per fork commit, its message saying why.
- A bump: rebase onto upstream, drop patches upstream made obsolete,
  rebuild with `bash awesome/gate/fork.sh`, port the plugins, run `-b all`.
  `fork.sh` reconfigures first: `src/version.h` and pkg-config's cached
  dependency versions are fixed at configure time, and either stale fails
  the plugins' load guard (2026-11-04).
- Dependencies are distro packages, except Aquamarine:
  `~/repo/PKGBUILDs/aquamarine` (the `flake.lock` pin and its patches).
  `fork.sh`, the gate's staged headers and its nested use that build
  (`gate/host.sh`).
- The user installs and deploys (the fork: install, relog, then `hyprpm
  update`).
- Two machines: a ThinkPad (x86, this host) and an M3 Max laptop (Asahi,
  scale 1.5, a P3 panel), reached only through its own agent session
  (`~/repo/asahilinux`). Hardware-shaped behavior detects the machine at
  runtime, never hard-codes one machine's values.

## Build

- `make -C awesome`: the release plugin (what hyprpm ships). `make -C
  awesome GATE=1`: `awesome-gate.so`, the gate's variant, through a
  precompiled header (`core/pch.hpp`); the release stays plain, so a
  missing include fails there.
- Test seams are compile-time only (`#ifdef AWESOME_GATE`), never an
  environment switch in the release binary.
- No `-fvisibility=hidden`: `plugin.ver` localizes plugin symbols, while
  Hyprland's inline globals must stay unified for `dlopen`.
- Builds stamp their target (compiler, flags, the headers' `version.h`):
  another header set rebuilds by itself; no `make -B`.
- Unit tests: `make test` (both plugins). vclick's nested check: `make
  vclick-check` (it takes the gate's lock).

## The gate

`make -C awesome gate ARGS="-s ~/repo/Hyprland [-b quick|all|LIST]"`
stages the fork's headers, builds the gate variant and runs the batteries
in a nested Hyprland: `quick` (default) for the dev loop, a battery list
for a focused change (`-b windows` for geometry or CSD), `all` for a bump
or a release (it adds the release build's deploy rehearsal). Before a push
and after any history rewrite, `bash awesome/gate/sweep.sh origin/main
HEAD` builds and unit-tests both plugins at every unpushed commit.

- Keep it lean: retire battle-tested checks, gate new behavior with a
  discriminating check, wait on state, never the clock
  (`awesome/docs/testing.md`, Waiting). `-p` profiles a run.
- Isolation is absolute: the nested runs in a private headless labwc
  (`gate/host.sh`) with its own session and system buses (a fake logind
  and backlight) and scratch XDG dirs. The harness sends the live session
  nothing — no output, workspace, window, dispatch, focus, cursor,
  brightness or volume change — and reads it only for the isolation check,
  which fails when the live outputs change or a nested window shows there.
- One run at a time, in tmux. Scratch under `/tmp/hypr-gate/` (tmpfs),
  runtime state under `$HYPR_HARNESS` (default
  `~/.local/share/hypr-nested`); nothing in `~`.
- Memory is capped: every entry point runs in its own scope
  (`gate/memcap.sh`); a hand-run build or test goes through `systemd-run
  --user --scope -p MemoryMax=1536M -p MemorySwapMax=0`, serially. A host
  OOM kills the live compositor (2026-10-26).
- While a gate runs, never edit gate scripts, rebuild a plugin or launch
  or stop by hand: bash reads battery scripts as it goes, and the nested
  maps the `.so`.

## Safety — hard lines

Explicit user instructions override these; they bind the agent's own
judgment.

- The live session is the user's: no `hyprpm update`/`enable`/`reload`,
  plugin unload, config reload or session exit by the agent, and no
  dispatches into it unless the user asked for live debugging.
- Never hot-swap a loaded plugin or overwrite a mapped `.so` in place
  (invariants 2 and 5).
- History: clean beats append-only. Amend, rebase and force-push
  (`--force-with-lease`) when the user allows it; push a backup tag first
  (a purge keeps its backup local: a pushed one keeps what it removes)
  and verify the unpushed tail (`git merge-base --is-ancestor`). Preserve
  the user's worktree changes.
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
   through `Awesome::CHop`; teardown resets listeners before hops and
   makes newly armed hops no-ops.
7. Plugin input emissions run before compositor session-lock checks:
   every input listener checks `Awesome::sessionLocked()` first and
   clears swallow masks, held counters, drag state and armed zones there.
8. Shutdown is a half-dead state: `CCompositor::cleanup()` sets
   `m_isShuttingDown` before it unloads plugins. All event-driven work
   no-ops once it is set: the supervisor gates its listeners on
   `Awesome::compositorShuttingDown()`, and a per-window or per-device
   listener only queues work for a hop.

## Compositor integration

- Behavior follows AwesomeWM's vanilla semantics, source-verified
  (`awesome/docs/design.md`), tuned where an X11 assumption is broken or
  unsafe on Wayland; every deviation says why. No per-app rules.
- Use the fork's public APIs and the native renderer and input ownership.
  Read private compositor state only where the target requires it, and
  document the dependency in code.
- Native layer surfaces, popups, IME surfaces, input-capture sessions,
  seat grabs and implicit pointer grabs stay authoritative: recheck the
  native hit-test stack before intercepting a press, and pass an active
  input-capture session through.
- Scanout: never edit direct-scanout or solitary state from the plugin; a
  layer that must paint over fullscreen says so (`ILayer::overFullscreen`,
  answered through `monitor.blockSolitary`).
- Render only after the warm/draw gate, damage and scissor correctly, and
  keep stable geometry for the bar, menus, cards and hit regions.

## Code conventions

- Extend the `core/` helper that owns a concern instead of recreating it
  (spawning: `core/proc.hpp` + `Jobs`; deferral: `CHop`; drawing: the
  canvas; persistence: `StateStore`; pixel work off the compositor
  thread: `CRasterWorker`; time-of-day deadlines: `CWallTimer`).
- Module symbols live in their namespace (`Awesome::Shell`, `::Windows`,
  `::Notify`, `::System`), platform symbols in `Awesome`; the global
  `PHANDLE` is a documented ABI exception. Namespaces are PascalCase with
  no `N` prefix; types keep upstream's `C`/`S`/`I`/`E` prefixes.
- D-Bus stays asynchronous and off render and input hot paths
  (`CBusLink::post()`, `pollSoon()`); never drain a connection inline.
- Bound every externally sized structure (queues, children, payloads,
  images, textures, caches) and define behavior at the bound.
- Deferred replies, timers, subprocess callbacks, menu sessions and
  replaced device or daemon state carry a generation or ownership check;
  teardown invalidates late callbacks before releasing their objects.
- Cross-module state travels through a documented core seam, never
  through another module's private state.
- Input, render and bus callbacks stay short; per-frame and per-motion
  paths allocate nothing they can cache.
- Comments only for hard constraints, non-obvious workarounds, magic
  values, cross-file contracts and regression guards.

## Modules

One plugin, five modules, one input pipeline, no inter-module bus.

- `core`: supervisor, input pipeline, hops, canvas (warm/draw gate),
  config schema, stores, process runner and jobs, raster worker, fd.o bus
  client.
- `shell`: the bar (workspaces, tasks, tray, bell, battery, clock,
  menubar launcher); owns bar and menu input.
- `windows`: the window state machine and all placement
  (maximize/minimize/restore, click and focus policy, spawn placement,
  snap).
- `notify`: the `org.freedesktop.Notifications` daemon (cards,
  conversations, shade, popups, inline reply, sound).
- `system`: volume and mic (wpctl), brightness (logind), touchpad policy;
  every action's feedback is a notify card.

The supervisor dispatches button, move, axis and key in the order
`awesome/main.cpp` registers the modules — shell, notify, windows, system
— and that order is a contract (the bar claims its strip and menus before
window policy; notification input beats the window below), agreeing with
`awesome/docs/design.md`.

## Git and versions

- One logical change per commit; imperative `scope: summary` subject of
  about 50 characters; scopes `awesome:`, `vclick:`, `devtools:`, `docs:`,
  `build:`, `all:`. No versions in subjects, no `Co-Authored-By`.
- Versions are MAJOR.MINOR.PATCH (redesign / feature / fix), in lockstep
  with `hyprpm.toml`.
- Never push a change that has not passed its build and the applicable
  gate tier.

## Definition of done

1. The docs (each plugin's `README.md` and `docs/`, the root `README.md`)
   and `TODO.md` reflect the actual behavior.
2. The plugins build against the exact target headers (both `awesome`
   variants) and `make test` is green.
3. The applicable gate tier ends with `ALL N CHECKS PASSED`, isolation
   included.
4. Teardown, reload, session lock, native input precedence, damage, queue
   bounds and late replies were considered for the change.
5. Commits and push results are reported accurately; no live-session
   operation was performed by the agent.
