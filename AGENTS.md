# hyprland-plugins — working agreement for coding agents

Native C++26 Hyprland plugins inspired by AwesomeWM: one directory per
plugin, shared code in `common/`, the controlled nested-compositor gate
in `devtools/`, behavior docs in `docs/`.

This file is the agreement between user and agent. It states principles
and hard lines, not project state. Broad user autonomy ("do your best")
never overrides the hard lines — they define what "do your best" may
touch. Where this file is silent, match the codebase's existing style
and use judgment; when scope is ambiguous, ask before expanding it.

Maintenance: add a rule only when an incident earns it, with the reason
attached; prune when the reason stops applying; keep the file lean — if
a section needs frequent updates, its content belongs in a source of
truth, not here. `CLAUDE.md` is dead — do not recreate or track it.

## Where state lives

- Plugin versions: `hyprpm.toml` (must equal `PLUGIN_INIT` in each
  plugin; the gate enforces).
- Fork state: `~/repo/Hyprland` — `git log` there is the truth for what
  the running binary is built from. Pre-bump states are tagged
  (`pre-bump-*`), so the fork is always restorable.
- Open issues and evidence limits: `TODO.md` (local, gitignored). Close
  an item by deleting its line; provenance for closed work is the
  commit history.
- Behavior: the plugin READMEs and `docs/hyprbar.md` /
  `docs/hyprnotify.md`.
- Gate results: the summary line of the run log
  (`== stress: ALL N CHECKS PASSED in Ns ==`), not shell exit codes.

When in doubt, verify against these sources instead of memory or
conversation state. Never copy state into this file.

## Before editing

- Check `git status` and preserve existing worktree changes.
- Read the affected plugin's README, module header, and entrypoint; for
  cross-cutting UI, renderer, tray, notification, or bus work read the
  focused docs; for compositor integration read the exact fork sources
  in `~/repo/Hyprland`.
- Keep scope user-driven: no new feature, product-model change, or
  protocol-contract change just because an alternative seems preferable.

## Environment and build

Plugins are ABI-locked to the exact `hitori-chan/Hyprland` fork commit
the compositor is built from; never assume upstream `main` is
compatible. A fork bump means re-checking the ABI surface (PluginAPI,
input capture/EIS, IME and native hit testing, renderer pass insertion,
monitor scanout hooks), rebuilding all plugins, and running the full
gate.

- Build with `make -C <plugin>`; `common/common.mk` owns the C++26 and
  ABI-sensitive flags. Do not add `-fvisibility=hidden`:
  `common/plugin.ver` localizes plugin symbols while Hyprland inline
  globals must stay unified for `dlopen`.
- The fork exposes a few members publicly that upstream keeps private
  (e.g. `CWaylandBackend::m_resource`, `CX11Backend::m_xwaylandSurface`)
  so plugins can read client state — read-only, documented in the fork
  headers.
- Dependencies come from distro packages (aquamarine 0.15.0; the
  `~/repo/aquamarine` checkout is exactly upstream, not a fork
  dependency); keep `~/repo/<dep>` checkouts at the fork's `flake.lock`
  pins; the user performs sudo installs.
- For an uninstalled fork, pass one package/header set through both
  `PKG_CONFIG_PATH` and `HYPR_DEPLOY_PKG_CONFIG_PATH` (same directory);
  never substitute a stale installed cache — watch stale-header
  resolution into `/usr/local/include` (dual-root redefinition errors)
  and the `-MMD` gap documented in `common/common.mk` (`make -B` after
  a header install).
- Long builds and gate runs go in tmux. Every temporary artifact —
  logs, debug dumps, scratch files, probe dirs — goes under a
  dedicated `/tmp` subdirectory (e.g. `/tmp/hypr-gate/`); keep `~`
  free of temp stuff. `/tmp` is tmpfs, so nothing
  that must survive a reboot goes there (fork work, large `gcore`
  dumps); fork work happens in `~/repo/Hyprland` itself. `cmd | tee`
  swallows the exit code — trust the log's summary line.
- The nested harness parks a headless `nested-dev` output in the live
  session; if workspace switching misbehaves after gate runs, check
  `hyprctl monitors all -j` before suspecting a plugin.

## Safety — hard lines

- The agent never operates the live desktop: no `hyprpm update`/
  `enable`, plugin unload, live reload, or session exit. Deploy is the
  user's action (build + relog).
- Never hot-swap a loaded plugin or overwrite a mapped `.so` in place
  (invariants 2 and 5). Let `hyprpm` own deployment, or build a
  complete artifact and rename it atomically.
- No destructive git operations (`reset --hard`, `checkout --`, branch
  deletion, force-push, rebase, history rewrite) without an explicit
  user request; preserve the user's worktree changes.
- Never rebuild or edit a plugin while a nested instance has it mapped.
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
   `common/texcache.hpp`'s warm/draw gate, create textures from the
   event loop, scissor paints to damage, and damage every
   visible-state transition (hover damages but does not rewarm).
5. Never overwrite a mapped `.so` in place.
6. Defer workspace and focus changes out of input emissions through
   `NHyprCommon::CHop`; teardown resets listeners before hops and makes
   newly armed hops no-ops.
7. Plugin input emissions run before compositor session-lock checks;
   every input listener checks `NHyprCommon::sessionLocked()` first and
   clears swallow masks, held counters, drag state, and armed zones
   there.

## Compositor integration

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
  correctly, and keep stable geometry for bars, menus, cards, OSDs, and
  hit regions.
- Prefer universal behavior; a per-app rule is explicit configuration,
  not a substitute for correct Wayland protocol ordering.

## Code conventions

- Extend the `common/` helper that owns the concern instead of
  recreating it.
- Plugin symbols live in the plugin namespace (`NHyprbar`,
  `NHyprnotify`, …); shared symbols in `NHyprCommon`; a required global
  `PHANDLE` is an ABI exception and must be documented.
- Keep D-Bus asynchronous and off render/input hot paths:
  `CBusLink::post()` for bus-originated work, `pollSoon()` for
  event-loop dispatch; never drain a connection inline.
- Bound every externally sized structure (queues, child-process work,
  action payloads, image data, decoded textures) and define behavior at
  the bound.
- Deferred replies, timers, subprocess callbacks, menu sessions, and
  replaced device/daemon state carry a generation or ownership check;
  teardown invalidates late callbacks before releasing their objects.
- Cross-plugin state travels through Wayland protocol state or a
  documented bus API, never through shared plugin symbols.
- Keep input, render, and bus callbacks short; move blocking work off
  compositor dispatch where the target API permits.
- Comments only for hard constraints, non-obvious workarounds, magic
  values, cross-file contracts, and regression guards.

## Plugin ownership and load order

- `hyprbar`: top strip, widgets, tray, dbusmenu, menubar, bell; owns
  bar/menu input; talks to `hyprnotify` through its bus API.
- `hyprnotify`: `org.freedesktop.Notifications`, banners, shade, DND,
  notification input; an action closes the shade.
- `hyprmax`: client-told per-window maximize, remembered windowed size.
- `hyprclick`: click/focus policy.
- `hyprsnap`: drag edge snapping and snap indicators.
- `hyprplace`: remembered per-class spawn geometry, least-overlap
  fallback; client grants stay authoritative.
- `hyprpad`: in-process touchpad policy via aquamarine and Lua config.
- `hyprosd`: volume/brightness bus client shown through `hyprnotify`;
  draws nothing.

The order in `hyprpm.toml` is a behavior contract:

`hyprbar -> hyprnotify -> hyprmax -> hyprsnap -> hyprclick -> hyprplace -> hyprpad -> hyprosd`

The bar claims its strip and menus before window policy; notification
input beats the window below; the maximized-window swallow beats
click-to-raise. `NHyprCommon::mustLoadBefore` and the plugin READMEs
must agree with the manifest.

## Git and versions

- Published history is append-only: fast-forward pushes are normal;
  amend, rebase, retag, and force-push only on explicit user request
  (first verify the unpushed tail with `git merge-base --is-ancestor
  origin/main main`).
- One logical change per commit; imperative `scope: summary` subject of
  about 50 characters; scopes: plugin names, `common:`, `devtools:`,
  `build:`, `all:`. No versions in subjects, no `Co-Authored-By`.
- Versions are MAJOR.MINOR.PATCH (redesign / feature / fix); keep
  `PLUGIN_INIT` and `hyprpm.toml` in lockstep (`hyprnotify` stores its
  source version in the shared header, the others in `main.cpp`).
- Keep behavior docs current with code; update `TODO.md` when a change
  affects an open item.
- Never push a change that has not passed its build and the applicable
  nested checks.

## Definition of done

Before calling work complete:

1. The affected README/docs and `TODO.md` reflect the actual behavior.
2. Each changed plugin builds against the exact target headers with
   C++26.
3. The relevant nested checks end with `ALL CHECKS PASSED`.
4. Teardown, reload, session-lock, native input precedence, damage,
   queue bounds, and late replies were considered for the change.
5. `git diff`/`status`, commits, and push result are reported
   accurately; no live plugin operation was performed by the agent.
