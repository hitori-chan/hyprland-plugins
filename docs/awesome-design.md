# awesome — design

One C++26 Hyprland plugin, built against the exact
`hitori-chan/Hyprland` commit the compositor runs: same process,
PluginAPI and fork headers, no FFI. It depends on a small, named fork
surface — `monitor.blockSolitary` (paint over a fullscreen window),
`reservedChanged` (usable-area changes), `fullWindowFocus` landing the
exact window across workspaces, compositor-minted activation tokens,
and the born-fullscreen float's windowed-size ask — and assumes the
fork's compositor fixes (activation as urgency, the CSD content frame,
the true maximize state, popup positions in window-geometry space, the
session-lock grab check). The behavior contract is the gate (`-b all`,
its summary line) plus [`awesome/README.md`](../awesome/README.md),
which owns everything user-facing: behavior, the Lua API, `hyprctl`
verbs, config keys and the state file. This document owns the
architecture and the reasons. It replaced eight plugins in 2026-09,
cut by domain rather than by the old plugin names; the commit history
has the provenance.

## 1. Modules

| module | owns |
|---|---|
| `core` | the platform (section 2) |
| `shell` | the persistent chrome: workspaces, tasks (views), tray, bell, battery, clock, launcher |
| `windows` | the window state machine and all placement: normal/minimized/maximized (told-state, adopt, restore), focus policy (vanilla activation semantics, raise, corpse guard, cycling, the urgent-restore hop for minimized asks), spawn placement (lastspot, least-overlap, fixed-size exclusion), drag snapping (arm/preview/commit, indicator layer) |
| `notify` | the `org.freedesktop.Notifications` daemon, the card model (conversations, grouping, DND, OSD band 9990s), popups, shade, inline reply, sound |
| `system` | machine-state controls — volume/mic (wpctl), brightness (logind), touchpad policy (mouse presence) — each action renders its feedback as a notify capsule (fixed IDs, a glyph, a level) |

Cross-module rules:

- Modules talk through `core` interfaces and direct C++ calls; there is
  no bus between them. The D-Bus the plugin speaks is external only:
  the fd.o daemon (`notify`), the SNI tray and dbusmenu (`shell`), and
  system-bus clients (logind brightness and BlueZ in `system`,
  power-profiles in the battery widget).
- A module never reaches into another module's state; the contract is
  the interface in `core` (`shell` asks `windows.minimize(w)`, `system`
  posts cards through the `notify` model API).
- The input order is the module registration order in `main.cpp` —
  `shell` → `notify` → `windows` → `system` — and the supervisor
  dispatches every event in it: the bar claims its strip and menus
  before the cards, the cards before window policy. The README states
  it; the gate's pipeline battery pins it.

## 2. core — the platform

Single-threaded by the fork's guarantee: the compositor event loop
drives input, dispatch, frame callbacks, and rendering. No locks on
plugin state; ordering is program order plus the hop queue. Two
workers are walled off: the Store's disk writer (a serialized snapshot
through a mutex, file syscalls only) and the raster worker (bytes in
memory only); neither reads plugin or compositor state.

- **Supervisor** — the one `PLUGIN_INIT`/`PLUGIN_EXIT`. Start: load the
  state file, start the desktop index and the raster worker, init the
  modules in registration order, then the input and render listeners
  and the font preload; a throw anywhere in that block undoes all of it
  (a throwing start ejects the plugin without `PLUGIN_EXIT`, and nothing
  may be left pointing into the unmapped `.so`). Stop: the listeners
  first, then the hops (cancelled, newly armed ones no-ops — invariant
  6), each module's persist and the state flush (the windows still
  open), the font preload, module teardowns in reverse order (each
  closes its own bus), then Jobs, the raster worker and the desktop
  index. Every state listener is gated on
  `compositorShuttingDown()` (invariant 8).
- **Hop queue** — the plugin's single `doLater` path (invariant 6).
  Every deferred callback captures the session generation; the queue
  drops callbacks from a dead generation. Modules have no direct
  `doLater` access.
- **Input pipeline** — one compositor listener per event class
  (pointer button/motion/scroll, key). The head: an event upstream
  already cancelled passes; session lock or a native input-capture
  session → every module drops its partial input state (swallow masks,
  held counters, drag state, armed zones — invariant 7) and the event
  passes. Then the chain, in the input order, with consume/pass
  semantics. Native ownership past that is each module's own recheck
  before it intercepts a press: seat grabs, implicit pointer grabs, and
  the native hit-test stack (layer surfaces, popups, IME). A claimed
  MOVE still reaches the later modules (with the claim set) so they let
  go of hover and pointer ownership: motion is state, not an action. The
  swallow masks are per button, not per device, so the head lets a
  release through while a press of that button reached the compositor
  (another device's): a swallowed release there left the button held,
  the app saw it stuck and the bar ignored every press. `hyprctl
  awesome trace` prints who took each of the last 32 events.
- **Canvas** — per-monitor scene. Modules contribute layers (bar
  strip, menus, card stack, shade, OSD capsules, snap indicator) with
  stable geometry, at an explicit level (`eLayerZ`: bar, cards, snap,
  popups) whose paint order agrees with the input order — where two
  overlap, the one that takes the press is on top (an open menu over
  the cards); one pass element per layer and monitor, scissor-painted
  to damage, under the warm/draw gate: a layer cannot paint a texture in
  the frame that created it (invariant 4). Textures warm on the event
  loop and draw no earlier than the next frame; every visible-state
  transition damages both old and new geometry (hover damages but does
  not rewarm). A warm can be scoped to one monitor and one layer (a
  launcher keystroke warms the prompt's layer on its monitor, nothing
  else); the cards warm on their own changes only, and only a stale
  rewarm (a draw that found a texture missing) forces every layer — at
  most 8 times in a row, so a texture no warm can build never loops the
  frame. A raster the cache refuses is remembered as empty, a hit that
  paints nothing.
- **Text** — the bar renders labels through the compositor's
  `renderText`; the cards shape with their own Pango layouts (the
  whitelisted markup subset, the literal-`<`/`&` rescue, `<a href>`
  hit-testing — http(s) and mailto only, a mailto's query dropped). Text
  that must be one line IS one line: the bar replaces a client label's
  control characters before rastering, the cards lay a single line out
  in Pango's single-paragraph mode, and every card raster is clamped to
  its slot (stacked combining marks reach far past the line box). One
  shared text engine for both is open work. Both share this thread's
  Pango font map and one set of font options (`core/text.hpp`: full
  hinting, hinted metrics, whole-pixel glyph positions) for every
  raster, measured and drawn alike — the compositor's `renderText` sets
  none, and its fractional placement read soft beside a terminal at 1x.
  A face's first layout loads it (~10 ms a weight
  and size): `core/fontwarm.hpp` loads the faces the boards use (both
  modules' families, four weights, the type roles' sizes, each
  monitor's scale, a first emoji) one per event-loop turn, 1.5 s after
  start, so a session's first notification does not pay them.
- **Icons** — one freedesktop name resolver in `core/icons.hpp` for the
  bar and the cards (GTK theme → hicolor → pixmaps; plausible names
  only; a two-generation memo). A theme dir with a fresh
  `icon-theme.cache` answers from GTK's own index: a hash bucket and a
  few `pread`s on a held fd (never mapped — a cache truncated under a
  mapping would SIGBUS the compositor), every offset bounds-checked, the
  walk's own preference order applied to the listed dirs, the winner
  stat'ed; a dir is re-probed every 5 s and a changed cache drops the
  memo's misses. The walk it replaces cost 35 ms (warm) to 180 ms (cold)
  for a name breeze and hicolor both lack. A dir without a cache is
  walked, at most 8 walks per 250 ms, past which a lookup is deferred:
  nothing, uncached, and the caller retries it a beat later — a burst of
  distinct names costs a bounded stall; a reload flushes the memo only
  on a theme switch. One file admission: a file is opened ONCE,
  non-blocking, and judged on that fd (a regular file under 32 MiB,
  2 MiB for an SVG, a bounded declared raster); a synchronous decoder
  decodes the bytes read from it, the cards' async decodes (the
  compositor's resource gatherer, 24 bounded slots, one decode shared by
  every card asking for the same file in a warm) read /proc/self/fd of
  the held file — never the path again. The bar still decodes its task,
  tray, menu and launcher icons synchronously, budgeted: a file held to
  1024 px a side, at most two large decodes (past 256² px, an SVG past
  64 KiB) per 250 ms, all decodes together 4 ms per 16 ms, the rest
  deferred like a refused walk; a tray pixmap is scaled to twice its
  cell as it arrives.
- **Desktop index** — one scan of the XDG application dirs for the
  whole plugin (`core/desktopindex.hpp`), shared by the bar's icons, the
  cards' sender identities and the launcher. The traversal runs in a
  bounded, cancellable helper process (`core/fileindex.hpp`: a plugin
  cannot leave a filesystem thread alive across `dlclose`); the event
  loop drains its framed records, backpressure pauses it (a frame too
  large for the queue's room waits whole), and teardown closes the pipe
  and signals its private process group through a pidfd, joining no
  filesystem work. A refresh builds aside and swaps in whole; a scan cut
  short (its 30 s deadline, a failed or killed helper) swaps nothing and
  is retried. The dir stats that decide a refresh still run on the
  compositor thread (TODO).
- **Config schema** — one declarative constexpr table: key, module,
  type, default, range (`core/schema.hpp`; the keys are listed in the
  README). A getter's key literal is checked when the call COMPILES
  (`CfgKey`, consteval): a typo is a build error, never a silent zero,
  and a read is an index into the registered values. Out-of-range values
  clamp, non-finite doubles fall back to the default, strings are
  bounded (4 KiB, cut at a codepoint) — the same pure functions the unit
  tests run. The defaults are the Frost tokens (`core/tokens.hpp`).
- **Store** — the plugin's single persistent state: one typed file
  (`$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv`; its row kinds
  are the README's State table), one durable atomic write (a private
  0600 temp, never through a link, fdatasync, rename, directory fsync)
  run by a writer thread (a disk stalled behind a heavy writer never
  stalls the compositor; the newest snapshot wins, and the stop's save
  joins it); all rows admitted or skipped (fixed file/row/key/entry
  bounds; a hostile file can never take the session down; the least
  recently remembered row goes at a bound). The six kinds (spawn spots,
  last-windowed boxes, launcher counts, prompt history, the card menu's
  per-app switches and chat mutes) are one in-memory `AppState` owned by
  the core's `StateStore` — the documented cross-module seam for
  persisted state. An unreadable file blocks writes.
- **Processes** — `core/proc.hpp` is the one spawn primitive
  (`pidfd_spawnp`: an empty signal mask and default dispositions,
  stdin/stderr on /dev/null, every fd above 2 closed, a race-free pidfd,
  the exit status read from the pidfd under the compositor's
  SA_NOCLDWAIT, where the kernel still has it). `Jobs` runs the
  plugin's helpers on it in class budgets — CONTROL (wpctl, 16, killed
  after 5 s), SOUND (4, its process group killed after 30 s), OPEN
  (xdg-open, 32, never killed); a deadline holds until delivery (a
  grandchild holding the pipe cannot keep a slot), callbacks never
  unwind into the C dispatch — and the file index's helper spawns
  through it too, its group signalled through the pidfd. The launcher's
  `Exec=` goes through the compositor's executor (exec rules,
  activation tokens).
- **Raster worker** — `core/raster.hpp`: one on-demand thread for pixel
  work on bytes already in memory (unpack, scale, crop, tint), so no
  image burst runs on the compositor thread: notification images past
  512×512, decimated to within 2x of the card first. Work captures only
  its own buffers and atomic pointers (never a file, the GL context or
  compositor state); it starts on demand, exits when idle, and teardown
  joins one job. Dones run on the event loop, are matched to their card
  by a ticket, and upload in the card's next warm. A full queue (32
  jobs, 64 MiB of payloads) refuses and the caller works inline; past
  4 MP one inline scale runs per 100 ms and the rest wait for a later
  warm.
- **Bus** — the fd.o Notifications daemon (spec 1.3), kept
  asynchronous off hot paths (`post()` for bus-originated work,
  `pollSoon()` dispatch, a dispatch capped at 64 messages and 4 ms).
  `Notify` is a raw method handler: sdbus-c++'s typed read copies each
  hint Variant element by element into a message of its own (0.2–0.65 s
  for a screenshot's image-data). The image header is validated first
  and its pixels read zero-copy, then copied once (only the bytes the
  unpack reads); only basic-typed hints become Variants, 64 at most;
  every other value is walked past without a copy — a trivial array as
  one block however deeply it is wrapped, a string as a pointer.
  sd-bus can leave no container unread and serves no block from a
  message that needs byte-swapping, so a walk is linear in elements and
  a big-endian sender's image is read byte by byte (~0.1 µs each): past
  65,536 walked elements (~17 ms), or 256 KiB of a big-endian image
  (~26 ms; a header declaring more is refused before the walk), the
  call is refused with a D-Bus error, the defined behavior at a bound
  only a hostile sender reaches. The daemon's signals leave in order from one queue; the
  action, the activation token and a reply go to the sender only. The
  tray installs ONE async match per signal interface, routed by sender
  and path, never a per-proxy (synchronous) AddMatch.
- **Bounds** — every externally sized structure has a bound and a
  defined behavior at it, enforced where it enters: client-fed queues
  are `CHopQueue<T, N>` (a push at the bound is refused), wire strings
  are cut at a codepoint boundary (`clipToUtf8Boundary`,
  `BoundedString<N>` where a string is stored as one), image data is
  decimated while unpacked, raster caches are capped in bytes, and work
  is bounded too (theme walks and large bar decodes a window, decodes a
  reply, messages a dispatch, elements a Notify walk).
  `BoundedQueue<T, N>` (eviction policy in the type) is the helper for
  an evicting queue.

Crash-class invariants (numbered in AGENTS.md) → structure:

| class | guarantee |
|---|---|
| 1 window list | no module interface exposes compositor window-list mutation |
| 2 plugin unload | hyprpm owns deployment; teardown cancels hops before the buses close |
| 3 key releases | the pipeline head owns swallow masks; locked/capture resets all; no button release is cancelled while a press of its button that reached the compositor is held |
| 4 texture frame | the canvas enforces the warm/draw gate for every layer |
| 5 mapped .so | build process; never overwritten in place |
| 6 deferrals | the single hop queue; teardown cancels and no-ops arming |
| 7 lock precedence | the pipeline head checks the session lock first, always |
| 8 shutdown | every state listener no-ops once the compositor is shutting down |

## 3. Modules

### The look (Frost)

One material for every surface, its tokens in `core/tokens.hpp` (the
schema's defaults name them; `theme.lua` overrides them):

- Glass at three thicknesses over the compositor's blur: thin for the
  bar (over the wallpaper's precomputed blur: nothing else lies under
  it) and the launcher, regular for cards, the center and the capsules,
  thick for menus. Cards and menus add a soft drop, a light
  top edge and a 10 % white rim; a menu floats higher (a deeper drop).
- Ink is one white at four strengths (the title white for titles and
  the bar's text and glyphs, 78 % body, 55 % meta, 34 % empty); the
  accent `#00CCFF` is TEXT — the viewed tag, the focused task, a
  selection, the bell while its center is open. It fills only what is
  live: a caret, the reply's send circle, a progress bar (the snap
  preview is its 10 % tint and ring) — and marks one thing, the bell's
  unseen dot. Urgent is `#FF6159`, as text.
- Radii: cards 18, menus 12, rows and tags 6, buttons and app icons 9;
  windows keep the user's `decoration:rounding`.
- Glyphs are the boards' own outline icons (`core/glyph_shapes.hpp`,
  each parsed by the unit harness: librsvg refuses a malformed shape
  whole, and the glyph silently never draws), drawn white and tinted at
  raster time; the bell sits on the 16 px grid it rasters at, its stems
  on half-pixel centres. Icons and tray pixmaps raster at the monitor's
  device size, never a larger raster shrunk on the GPU.
- The bar's right end follows macOS 27's menu bar, measured off its 2x
  screenshot: 20 px between items, the bell SF Symbols' bell.badge with
  no count. The battery is Pixel's, SystemUI's Compose battery
  transcribed (`shell/battery_pill.hpp`, pure cairo): the number inside
  the body in its own vector digits. Each end of the strip owns its
  screen edge (Fitts), and every widget its cell: a widget with nothing
  to click swallows its clicks rather than lending them to a neighbour.
- Hover lifts text a step, never a ground: dim to the bar's text, and
  the accent (the viewed tag, the focused task) toward white.
- No surface darkens the desktop: the center draws no backdrop, and
  what floats over a wallpaper carries its own glass — or, for the
  center's "Notifications" heading, a soft text shadow.
- Cards follow macOS 27's notifications, without its hover buttons: no
  close button and no Options under the pointer (the user's hold and
  right click do that work; the glass alone lifts). The card menu (held
  left) lists the sender's rows (Reply, the actions) over the system's
  switches; an app's cards stack in the center under a deck. A hover
  damages, never rewarms (invariant 4).
- The mouse on a card: left acts on its release, a held left opens the
  card menu, right dismisses (a stack whole), middle does nothing.

### shell

The strip (workspaces, tasks, tray, bell, battery, clock), the menubar
launcher, and their input. Tasks are VIEWS of `windows` state;
minimize/restore/focus are requests to `windows`, not state operations
in the bar. The bell reads the `notify` model directly (the unseen
dot, DND, center state) and calls `notify.center()`; no bus. Launcher: the shared
desktop index, history, completion, 4 KiB UTF-8 paste limit.

### windows

One state machine per window: `normal | minimized | maximized`, with
the told-state (xdg `set_maximized`), compositor-grant adoption, and
workarea boxes, plus the focus policy (click-to-raise, keyboard
raise, corpse guard, cycling) and placement (spawn lastspot,
least-overlap, fixed-size exclusion, drag snap with indicator). The
window-press arbitration — swallow / focus / let the compositor drag
proceed — is one function. Maximized windows stay immovable (the press
is swallowed whole, with matching release swallow). Spawn positions
(the client keeps its own size) and remembered windowed sizes persist
in the core's state file (`spot` and `windowed` rows). The CSD
contract (fork-side, source-verified against upstream Hyprland's
content-frame model, the one its popup math already assumes): the
window box IS the client's content frame (its `set_window_geometry`
rectangle) and the client's min/max bound it as-is; the compositor
shows exactly that rectangle of the client's bigger buffer (content +
CSD shadow margin) at the box, cropping the margin — never padding
between the border and the content, never a blurred halo outside it —
and input maps through the same origin (the buffer sits at the box
minus the geometry offset), so the pointer lands on the pixel it is
over (2026-10-04: the surface-frame model shipped the margin inside the
box as blurry padding around firefox and the file picker; 2026-10-05:
with only the drawing cropped, upstream's buffer-origin hit test
shifted every click on a floating CSD window up-left by its shadow
margin).

### notify

The daemon and the model exactly as the behavior docs specify
(conversation merge, the 9990s band, grouping, DND, persistence,
ranking, the icon anatomy, the pixel model). Surfaces: the banners, the
center and the OSD capsules on the shared canvas — one card renderer
for the banner, the center and a card in a stack (`notify/stack.cpp`
draws the deck and the spread stack's header around it) — and the card
menu, drawn last and hit first on the notify layer. The menu's switches (an
app's banners and sounds, a chat's mute) persist through the core's
`StateStore` and gate the arrival, the DND resume and the re-pop alike.
Inline reply, sound through Jobs. The model API is the module's public
face — `system` posts its feedback cards through it (fixed IDs, low
urgency, a value, a glyph that makes it a capsule).

### system

Audio and brightness controls (bounded wpctl set/readback chains,
logind `SetBrightness`, /sys reads) and the touchpad policy (disabled
while a physical mouse is present — one that has moved or clicked —
400 ms coalesced recheck, manual toggle until hotplug, across config
reloads). Every action's feedback is a notify capsule: fixed IDs
replace in place (1200 ms; the touchpad's 1500 ms), a glyph and a
level or words. A Bluetooth headset's own volume change (BlueZ's
transport `Volume` on the system bus, no key) reads the sink back and
shows the capsule only when something moved. All process I/O through
Jobs; readbacks reject malformed output.

## 4. Interfaces

The README lists them (it owns them): the Lua API, one flat namespace
`hl.plugin.awesome.<fn>`; `hyprctl awesome <verb>`, routed to the
owning module (`trace` is the supervisor's; `drawstats` exists in the
gate build only); the config keys `plugin:awesome:<module>:<key>`; the
state file's rows. The pre-monolith names (`hyprctl hyprnotify …`, the
per-plugin `hl.plugin.*` functions, `org.hitori.hyprnotify`) are
removed, not aliased. Manifest: one entry, `awesome`; the version is
MAJOR.MINOR.PATCH, in lockstep between `core/version.hpp` and
`hyprpm.toml`.

## 5. Performance

The compositor thread runs on a realtime budget (rtkit's RLIMIT_RTTIME:
150 ms soft, 200 ms hard per burst); no plugin burst may approach it.
Measured in the nested gate (2026-10-25, plugin 3.0.1): a 1080p
image-data answers in 16 ms, 65 Notify calls at once peak at 20–52 ms,
a session's first card at 19 ms with Mesa's shader cache warm (cold,
as in every gate run, upstream's lazy shader-variant compiles add
~100 ms to the first card's draw).

- One canvas pass element per layer and monitor, each with its own box:
  the renderer culls a layer the frame's damage misses, and an element
  claims live blur only while its layer paints translucent glass (every
  Frost default is; an opaque `col_bg` draws plain and claims none).
- The bar's glass samples the wallpaper's precomputed blur instead
  (`ILayer::wallpaperGlass`; blurred once per wallpaper change, the
  pre-blur kept queued while the bar paints): the work area starts
  below it. A live blur re-blurred the strip on every repaint and, under
  partial damage, read the ring each swapchain buffer kept from its last
  frame, so over a terminal taking typing the presented frames
  alternated between two bars (upstream's FIXME in
  `CRenderPass::render`). A frame the precomputed blur is stale for (the
  first after load) falls back to the live one; over a fullscreen client
  (the open menubar) the strip stays live.
- Warms coalesce: one bar warm per event-loop turn however many events
  arrived, and a title change (the one input a client drives at will)
  warms the strip layer alone, at most once per 16 ms; the cards warm
  once, on their monitor, and only on their own changes (a clock tick
  or a title change no longer re-measures the shade); the launcher's
  keystrokes warm its layer on its monitor and damage the prompt band.
  The shade measures no row past the fold.
- Config reads are an index (the key resolved at compile time) and a
  typed value read — no hash, no allocation, no RTTI.
- No bus round-trips on any live path (the bell's dot, DND state, OSD
  posts are in-process calls), and no synchronous AddMatch anywhere.
- First uses are paid early or once: font faces by the preload, icon
  names from GTK's caches, image pixels on the raster worker; the
  plugin's load warms the bar once (~62 ms, most of it the CJK fallback
  for the kanji tags).
- The draw re-runs the layout per frame, deliberately: measured with the
  gate's `drawstats` (a card over a window redrawing every frame), the
  worst case is ~90 us for the card and ~90 us for the bar when damage
  reaches it — about 1% of a 60 Hz frame. A cached
  display list would save ~100 us at the cost of a stale-content risk
  across every state source.

## 6. The gate

- **Isolation** — the nested Hyprland runs inside a private headless
  labwc (`gate/host.sh`), with its own session bus, a private system bus
  carrying a fake logind and backlight (`devtools/fakes/fake-logind`),
  scratch XDG dirs, and aquamarine at the fork's `flake.lock` pin. The
  live session receives nothing; the isolation check (structural at each
  launch, observed at the end) fails the gate on any trace.
- **Two builds** — the release plugin (the deploy rehearsal) and the
  gate variant (`make GATE=1`, `awesome-gate.so`): test seams compile in
  under `AWESOME_GATE` only.
- **One command** — `make -C awesome gate ARGS="-s <fork> -b <tier>"`
  stages the fork build's headers itself and verifies its `version.h`.
- **Check engine** — `chk <desc> <cmd…>` (the command's status decides
  and is returned), `ok`/`bad`, and `skip` for a check whose
  precondition is missing: a skip is listed under the summary and never
  counted as a pass. Each battery reports its tally; a battery that ran
  no checks fails. A nested that dies is caught by its exit status (a
  signal death fails the gate; a crash report or the nested's evidence is
  retained), not by log greps. One run at a time: gate, probe and dev
  share a lock on `$HARNESS`. The summary line is the contract:
  `== stress: ALL N CHECKS PASSED in Ns ==`.
- **Batteries per module** — `shell`, `windows`, `notify`, `system`,
  `pipeline`, `lifecycle` (reload/teardown/lock/capture/queue bounds),
  plus `quick`, the default smoke. New behavior gets a discriminating
  check (it fails against the old code); battle-tested checks are
  retired once their code stops changing, and dropping one is a
  reviewable diff.
- **Unit tests** — one headless C++26 harness, `make -C awesome test`:
  no compositor runs (it compiles against the fork's headers for the
  hop's event-loop types; `test/stubs.cpp` supplies their symbols).
  Bounded types, box math, config schema (including the alpha-0 color
  guard and the clamps), store admission, wpctl parsing, Exec/launcher
  quoting, text cutting, the process runner (spawn under the
  compositor's signal state), the icon-theme cache reader.
- **Fixtures** — the C Wayland fixtures (vptr, vkbd, fake-sni, cliphold,
  fixwin, splashwin, input-capture) stay C against the exact fork
  protocol XML; fakes (wpctl, the sound player, logind) shadow the
  host's services; all live in `devtools/` and are extended only where a
  battery needs a new shape.
