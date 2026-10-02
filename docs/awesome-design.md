# awesome — design

One C++26 Hyprland plugin replacing the eight. Same process, same
PluginAPI, same fork headers — no FFI, no fork-side surface. The
behavior contract is the current gate (249 checks) plus the behavior
docs; a check may be re-scoped to the new architecture, never waived.

The module boundaries are re-derived from the domain. The old plugin
names are legacy and do not survive: the bar's minimize state
operation, the bell's bus interface, the max/click load-order glue, and
the osd/notify separation were symptoms of the old cut, and the new cut
removes them.

## 1. Modules

| module | owns |
|---|---|
| `core` | the platform (section 2) |
| `shell` | the persistent chrome: workspaces, tasks (views), tray, bell, battery, clock, launcher |
| `windows` | the window state machine and all placement: normal/minimized/maximized (told-state, adopt, restore), focus policy (vanilla activation semantics, raise, corpse guard, cycling, the urgent-restore hop for minimized asks), spawn placement (lastspot, least-overlap, fixed-size exclusion), drag snapping (arm/preview/commit, indicator layer) |
| `notify` | the `org.freedesktop.Notifications` daemon, the card model (conversations, grouping, DND, OSD band 9990s), popups, shade, inline reply, sound |
| `system` | machine-state controls — volume/mic (wpctl), brightness (logind), touchpad policy (mouse presence) — each action renders its feedback as a notify card (fixed IDs, value bar) |

Legacy → new:

| old | new home |
|---|---|
| hyprbar (chrome) | `shell` |
| hyprbar (minimize/restore state ops in tasklist) | `windows` |
| hyprmax, hyprclick, hyprplace, hyprsnap | `windows` |
| hyprnotify | `notify` |
| hyprosd, hyprpad | `system` |
| common/* | replaced by `core` (nothing copied wholesale) |

Cross-module rules:

- Modules talk through `core` interfaces and direct C++ calls. There is
  no bus between modules: `org.hitori.hyprnotify` is deleted. The only
  D-Bus the plugin speaks is the fd.o daemon (owned by `notify`) and
  client calls (logind) inside `system` jobs.
- A module never reaches into another module's state; the contract is
  the interface in `core` (e.g. `shell` asks `windows.minimize(w)`,
  `system` posts cards through the `notify` model API).
- The input pipeline (core) dispatches each event to the modules in a
  fixed, documented priority: `shell` → `notify` → `windows`. The old
  `mustLoadBefore` edges and the hyprpm load-order contract die; the
  registration order in `core/supervisor` is the contract, and the
  gate's pipeline battery pins it.

## 2. core — the platform

Single-threaded by the fork's guarantee: the compositor event loop
drives input, dispatch, frame callbacks, and rendering. No locks, no
atomics; ordering is program order plus the hop queue.

- **FrameContext** — per frame: frame number, session generation,
  session-locked flag, native input-capture flag, focused window,
  monitor list, the damage set. Built once per frame by the supervisor;
  every module and every input handler reads it, never re-queries.
- **Supervisor** — the one `PLUGIN_INIT`/`PLUGIN_EXIT`. Owns the module
  table: init order is the priority order, teardown runs reverse.
  Teardown: session-lock reset of all input state, hop queue cancelled
  and armed-becomes-no-op (invariant 6), module resets reverse order,
  store flush, bus close. One function, gate-tested.
- **Hop queue** — the plugin's single `doLater` path (invariant 6).
  Every deferred callback captures the session generation; the queue
  drops callbacks from a dead generation. Modules have no direct
  `doLater` access.
- **Input pipeline** — one compositor listener per event class
  (pointer button/motion/scroll, key). The head enforces, in order:
  session-locked → reset every swallow mask, held counter, drag state,
  armed zone and bail (invariant 7); native input-capture/seat grab
  active → pass through; native hit-test (layer surfaces, popups, IME)
  → pass through. Then the handler chain `shell → notify → windows`
  with consume/pass semantics. The chain is a table in one file.
- **Canvas** — per-monitor scene. Modules contribute layers (bar
  strip, menus, card stack, shade, OSD cards, snap indicator) with
  stable geometry; the canvas computes the damage union, runs glass per
  glass group, scissor-paints, and enforces the warm/draw gate: a layer
  cannot paint a texture in the frame that created it (invariant 4).
  Damage happens on every visible-state transition, including hover.
- **Text engine** — one Pango context, one layout pool, shared glyph
  metrics. The whitelisted markup subset, the literal-`<`/`&` rescue,
  `<a href>` hit-testing, and the card text shaping all go through it.
  No per-module pango state.
- **Icon subsystem** — freedesktop resolution (GTK theme → hicolor →
  pixmaps), the .desktop index, symbolic recolor, and ONE bounded
  decode queue on the compositor's async resource gatherer. Cards and
  bar icons share it; the warm/draw gate applies on decode land.
- **Config schema** — one declarative table: key, module, type,
  default, validator. Typed accessors; theme tokens resolved once;
  unknown keys are a load error. The legacy keys re-namespace to
  `plugin:awesome:<module>:<key>` (47: shell 20, notify 24, windows 3).
- **Store** — bounded key/value, list, and box stores with admission
  rules (fixed file, row, key, entry bounds; malformed state
  ignored). `$XDG_STATE_HOME/awesome/`: `windows-spot.tsv`,
  `windows-windowed.tsv`, `shell-launches.tsv`, `shell-history.tsv`.
  First run migrates the old per-plugin stores once (read old path,
  write new, never both).
- **Jobs** — the one subprocess runner: bounded queue, generation-
  checked callbacks, single reap path, env isolation. `system`'s
  wpctl/logind chains, the launcher's `Exec=` spawn, notify's sound
  player and link opens all go through it. Nothing else in the plugin
  spawns a process.
- **Bus** — the fd.o Notifications daemon (spec 1.3), kept
  asynchronous off hot paths (`post()` for bus-originated work,
  `pollSoon()` dispatch). The plugin-internal service interface is a
  C++ interface, not a bus.
- **Bounded types** — `BoundedString<N>` (UTF-8-aware) and
  `BoundedQueue<T,N>` (eviction policy in the type) at every wire
  boundary: bus strings, action payloads, image data, decoded
  textures, queue depths. "Bound every externally sized structure" is
  a type property, not a discipline.

Crash-class invariants → structure:

| class | guarantee |
|---|---|
| 1 window list | no module interface exposes compositor window-list mutation |
| 2 plugin unload | hyprpm owns deployment; teardown cancels hops before bus close |
| 3 key releases | the pipeline head owns swallow masks; locked/capture resets all |
| 4 texture frame | the canvas enforces the warm/draw gate for every layer |
| 5 mapped .so | build process; never overwritten in place |
| 6 deferrals | the single hop queue; teardown cancels and no-ops arming |
| 7 lock precedence | the pipeline head checks the session token first, always |

## 3. Modules

### shell

The strip (workspaces, tasks, tray, bell, battery, clock), the menubar
launcher, and their input. Tasks are VIEWS of `windows` state;
minimize/restore/focus are requests to `windows`, not state operations
in the bar. The bell reads the `notify` model directly (count, DND,
shade state) and calls `notify.center()`; no bus. Launcher: bounded
desktop-entry and command index, history, completion, 4 KiB UTF-8
paste limit.

### windows

One state machine per window: `normal | minimized | maximized`, with
the told-state (xdg `set_maximized`), compositor-grant adoption, and
workarea boxes, plus the focus policy (click-to-raise, keyboard
raise, corpse guard, cycling) and placement (spawn lastspot,
least-overlap, fixed-size exclusion, drag snap with indicator). The
window-press arbitration — swallow / focus / let the compositor drag
proceed — is one function, replacing the four load-order edges.
Maximized windows stay immovable (the press is swallowed whole, with
matching release swallow). Spawn geometry persists in
`awesome/windows-spot.tsv`, remembered windowed sizes in
`awesome/windows-windowed.tsv`.

### notify

The daemon and the model exactly as the behavior docs specify
(conversation merge, the 9990s band, grouping, DND, persistence,
ranking, the AOSP icon anatomy, the pixel model). Surfaces: popups and
the shade on the shared canvas. Inline reply, sound through Jobs. The
model API is the module's public face — `system` posts its feedback
cards through it (fixed IDs, low urgency, value bar).

### system

Audio and brightness controls (bounded wpctl set/readback chains,
logind `SetBrightness`, /sys reads) and the touchpad policy (disabled
while a physical mouse is present, 400 ms coalesced recheck, manual
toggle until hotplug). Every action's feedback is a notify card:
fixed IDs replace in place, 1200 ms expiry, native identities. All
process I/O through Jobs; readbacks reject malformed output.

## 4. API (no legacy)

Lua, one entry point:

```lua
hl.plugin.awesome.menubar()                -- the launcher
hl.plugin.awesome.maximize()               -- the focused window
hl.plugin.awesome.minimize()
hl.plugin.awesome.restore()
hl.plugin.awesome.focus_next()
hl.plugin.awesome.focus_prev()
hl.plugin.awesome.focus_prev_here()
hl.plugin.awesome.center()                 -- toggles the shade
hl.plugin.awesome.suspend()                -- toggles DND
hl.plugin.awesome.clear_all()
hl.plugin.awesome.volume_up()              -- volume_down, mute, mic_mute
hl.plugin.awesome.brightness_up()          -- brightness_down
hl.plugin.awesome.touchpad_toggle()
```

`hyprctl awesome <verb>` routes to the owning module: notify takes
`{count, center, state, badge, topline, clear}`, system takes `pad`;
the old `hyprctl hyprnotify …` and the per-plugin `hl.plugin.*`
names are removed, not aliased.

Config: `plugin:awesome:shell:*` (20 keys), `plugin:awesome:notify:*`
(24), `plugin:awesome:windows:*` (3: edge, snap_distance, col_frame).
The user's `theme.lua`/`binds.lua` got one migration pass at cutover.

Manifest: one entry, `awesome`; the version is MAJOR.MINOR.PATCH and
stays in lockstep between `core/version.hpp` and `hyprpm.toml`.

## 5. Performance

- One Pango context and glyph cache for all text (bar, cards, menus,
  OSD) instead of two independent shaping pipelines.
- One damage computation and glass grouping per monitor; layers share
  the stack's glass where the design allows it.
- One bounded icon decode queue on the async gatherer for bar and
  cards.
- No bus round-trips on any live path (badge counts, DND state, OSD
  posts are in-process calls).
- The pixel model's metrics are `constexpr` where they are fixed; the
  card geometry is computed once per state change, not per frame.

## 6. devtools — re-derived, not patched

The harness keeps its operational core (nested launch/teardown,
capture validation, the cgroup session-root rule, the flock display
check, the coredump teardown guard, the probe env). What changes:

- **Check engine** — one `check <id> <desc> <fn>` primitive with a
  machine-readable manifest: declared checks vs executed, per-battery
  coverage, failure screenshots auto-retained. The summary line is the
  contract: `== stress: ALL N CHECKS PASSED in Ns ==`.
- **Batteries per module** — `shell`, `windows`, `notify`, `system`,
  `pipeline`, `lifecycle` (reload/teardown/lock/capture/queue bounds),
  replacing the per-plugin batteries. Each old check maps to a new
  check id; the mapping table lives in the gate (`manifest.tsv`), and
  dropping a check is a reviewable diff.
- **Unit tests** — one headless C++26 harness (no external deps),
  `make -C awesome test`: bounded types, box math, config schema
  (including the alpha-0 color guard), store admission, wpctl parsing.
- **Fixtures** — the C Wayland fixtures (vptr, vkbd, fake-sni, cliphold,
  fixwin, splashwin, focustrap) stay C against the exact fork protocol
  XML; they live in `devtools/` and are extended only where a battery
  needs a new shape.

## 7. Execution (as landed)

Built in the order `core` → `windows` → `notify` → `system` → `shell`
→ the re-derived gate; the old tree stayed intact until cutover so the
regression net held throughout. The cutover was one commit series:
manifest flipped to `[awesome]`, the old eight directories and
`common/` deleted, the user config migrated to `plugin:awesome:*`,
AGENTS.md rewritten. It landed on `main` with the gate green and the
user deploy verified (2026-09-30); the provenance is the commit
history, not this document.
