# Design

How `awesome` is built and why. Behavior is in the module docs
([shell](shell.md), [windows](windows.md), [notify](notify.md),
[system](system.md), [reference](reference.md)), held to by the gate's
`-b all` ([testing](testing.md)). The crash-class invariants are numbered
in [`AGENTS.md`](../../AGENTS.md).

One C++26 plugin, built against the exact `hitori-chan/Hyprland` commit
the compositor runs: same process, PluginAPI and fork headers, no FFI.

**Fork surface used**: `monitor.blockSolitary` (paint over a fullscreen
window), `reservedChanged`, the plugin reserved type (the bar's band),
`fullWindowFocus` keeping its reason across workspaces, compositor-minted
activation tokens, the born-fullscreen float's windowed-size ask.
**Fork fixes assumed**: activation as urgency, the CSD content frame, the
true maximize state, popup positions in window-geometry space, the
session-lock grab check.

## Modules

| Module | Owns |
|---|---|
| `core` | the platform (below) |
| `shell` | the bar (tags, tasks, tray, bell, battery, clock) and the launcher |
| `windows` | the window state machine (normal, minimized, maximized), focus policy, spawn placement, snap |
| `notify` | the fd.o daemon, the card model, banners, the center, inline reply, sound |
| `system` | backlight (logind), volume and microphone (wpctl), touchpad policy; feedback as capsules |

- No bus between modules. A module calls the owner's API (`shell`
  minimizes through `Windows::Tasklist`; `system` posts through notify's
  `postCard`) or a `core` seam (`core/bands.hpp`, `StateStore`), never
  another module's state. The plugin's D-Bus is external only: the fd.o
  daemon, the SNI tray and dbusmenu, and system-bus clients (logind,
  BlueZ, power-profiles).
- Input order is the registration order in `main.cpp`: `shell` →
  `notify` → `windows` → `system`. The bar claims its strip and menus
  before the cards, the cards before window policy; the pipeline battery
  pins it.
- Pointer and keyboard only: touch reaches the native surface beneath.

## core

Single-threaded by the fork's guarantee: the event loop drives input,
dispatch, frame callbacks and rendering, so plugin state takes no locks.
Two workers are walled off and read no plugin or compositor state: the
store's disk writer (file syscalls on a snapshot) and the raster worker
(bytes in memory).

### Supervisor

- **Start**: load state, start the desktop index and raster worker, init
  modules in order, then the input and render listeners and the font
  preload. A throw undoes all of it: a failed start ejects the plugin
  without `PLUGIN_EXIT`, so nothing may point into the unmapped `.so`.
- **Stop**: listeners, then hops (invariant 6), each module's persist and
  the state flush (windows still open), the font preload, module
  teardowns in reverse (each closes its own bus), then Jobs, the raster
  worker and the desktop index.
- Every `supervisor().listen` listener is gated on
  `compositorShuttingDown()` (invariant 8).

### Deferral and time

- **`CHop`** is the plugin's one `doLater` path. Re-arming cancels only
  its own pending callback; after teardown `arm()` is a no-op.
  `CHopQueue<T, N>` batches pushes into one hop and refuses a push at its
  bound.
- **`CWallTimer`**: time-of-day deadlines (the clock's minute, a timed
  DND) on an absolute realtime timerfd, which fires at once after a
  suspend or a clock step; the loop's monotonic timers do not.

### Input pipeline

One listener per event class (button, motion, scroll, key):

1. An event upstream cancelled passes.
2. Under a session lock or a native input-capture session, every module
   drops its partial input state (invariant 7) and the event passes.
3. The modules, in input order, consume or pass.

Each module rechecks native ownership before taking a press: seat grabs,
implicit grabs, the native hit-test stack (layer surfaces, popups, IME).
A claimed motion still reaches later modules, flagged, so they drop
hover. Swallow masks are per button, so the head counts presses that
reached the compositor and passes their releases. `hyprctl awesome
trace` shows who took each of the last 32 events.

### Canvas

A per-monitor scene of layers (strip, menus, cards, center, capsules,
snap indicator) with stable geometry at a level (`eLayerZ`) whose paint
order matches input order.

- One pass element per layer and monitor with its own box, so the
  renderer culls and scissors per layer; an element claims live blur
  only while it paints translucent glass.
- **Warm/draw gate** (invariant 4): textures are made on the event loop
  and drawn no earlier than the next frame. Every visible-state change
  damages old and new geometry; hover damages without rewarming.
- A warm can be scoped to one monitor and layer. A stale rewarm (a draw
  found a texture missing) forces every layer, at most 8 times in a row.
- **Glass repaints whole**: a partial repaint of live blur samples the
  buffer's old glass and shimmers (upstream's FIXME in
  `CRenderPass::render`), so damage near a glass layer repaints all of it
  (`Canvas::wholeGlass`).

### Text

The bar rasters labels as `renderText` does; cards use their own Pango
layouts (markup, links). One-line text stays one line (control
characters replaced, single-paragraph mode, rasters clamped to the
slot). One font map and one set of options (`core/text.hpp`: full
hinting, whole-pixel glyph positions). A face's first layout costs
~10 ms, so `core/fontwarm.hpp` preloads faces one per 40 ms from 1.5 s
after start.

### Icons and the desktop index

**One icon resolver** (`core/icons.hpp`): the GTK theme (and its base
for `-dark`/`-light`) → hicolor → Adwaita → AdwaitaLegacy → `pixmaps`;
plausible names only; a two-generation memo.

- A fresh `icon-theme.cache` is read with bounds-checked `pread`s on a
  held fd, never mapped (a truncated mapping would SIGBUS the
  compositor). Dirs are re-probed every 5 s.
- Uncached dirs are walked, 8 walks per 250 ms; past that a lookup is
  deferred. A config reload flushes the memo only on a theme switch.
- **File admission**: one non-blocking open, judged on the fd (a regular
  file, < 32 MiB, < 2 MiB for SVG, a bounded raster). Decoders read that
  fd (or its `/proc/self/fd` path), never the path again.
- Cards decode on the compositor's resource gatherer: 24 jobs tracked,
  two in flight, one decode per file. The bar decodes inline under a
  budget: 1024 px a side, two large decodes per 250 ms, 4 ms per 16 ms.

**The desktop index** (`core/desktopindex.hpp`): one scan of the XDG
application dirs for the bar, the cards and the launcher, in a bounded
helper process (`core/fileindex.hpp`; a plugin cannot leave a thread
alive across `dlclose`). The loop drains its records with backpressure;
teardown signals its process group through a pidfd. A refresh swaps in
whole; a scan cut short (30 s deadline, a failed helper) swaps nothing.

### Configuration and state

- **Schema** (`core/schema.hpp`): one constexpr table of key, type,
  default and range. Keys are checked at compile time (`CfgKey`), reads
  are indexes. Out-of-range values clamp, non-finite doubles fall back,
  strings are cut at 4 KiB on a code point. Defaults are the Frost tokens
  (`core/tokens.hpp`).
- **Store** (`StateStore`, `core/store.hpp`): one typed state file
  ([reference](reference.md#state)), written atomically (private 0600
  temp, `fdatasync`, rename, dir fsync) on a writer thread; the newest
  snapshot wins and stop joins it.
- **Options** (`core/options.hpp`): menu choices, one schema row each,
  written only off default; saves coalesce per loop turn. Config files
  stay read-only to the plugin, and nothing is both a key and an option.
- **Bands** (`core/bands.hpp`): what lies across a monitor under the bar.
  `shell` publishes the launcher's strip; `notify` stacks under it.

### Processes and pixels

- **Spawning** (`core/proc.hpp`): `pidfd_spawnp`, empty signal mask and
  default dispositions, stdin/stderr on /dev/null, fds above 2 closed,
  exit status from the pidfd (the compositor runs `SA_NOCLDWAIT`).
- **Jobs** budget helpers by class: CONTROL (wpctl, the battery read, the
  app-dir probe: 16, killed after 5 s), SOUND (4, 30 s), OPEN (xdg-open:
  32, never killed). Every helper leads its own process group and is
  killed with it, so no pipeline stage outlives it. A deadline holds
  until delivery, piped output is capped, callbacks never unwind into C.
  The launcher's `Exec=` uses the compositor's executor (exec rules,
  activation tokens).
- **Raster worker** (`core/raster.hpp`): one on-demand thread for pixel
  work on in-memory bytes (notification images past 512×512, decimated to
  2x the card first). Results are matched to their card by ticket and
  upload in its next warm. A full queue (32 jobs), or 64 MiB of pending
  `image-data`, falls back inline; past 4 MP, one inline scale per
  100 ms.

### The notification bus

Asynchronous off hot paths: `post()` for bus work, `pollSoon()` dispatch
capped at 64 messages and 4 ms.

- `Notify` is a raw handler: sdbus-c++'s typed read copies each hint
  into its own message (0.2–0.65 s for a screenshot). The image header is
  validated, its pixels read zero-copy and copied once; only basic-typed
  hints become Variants (64 at most); the rest is walked past.
- sd-bus walks containers element by element and reads byte-swapped
  blocks byte by byte. Past 65,536 elements read, kept or walked (every
  dict entry counts, a repeated key too), or 256 KiB of big-endian image
  bytes in one call, the call is refused with a D-Bus error. Hints and
  action strings past the 64th are walked past; a card keeps eight
  actions.
- Signals leave in order from one queue; actions, tokens and replies go
  to the sender only. The queue holds a Clear All of `max_notifs` chats
  with full logs (512 × 33 signals) plus room for clicks.

### Bounds and invariants

Every externally sized structure is bounded where it enters, with
defined behavior at the bound: client-fed queues are `CHopQueue<T, N>`,
wire strings are cut on a code point (`clipToUtf8Boundary`), images are
decimated while unpacked, raster caches are capped in bytes, and work is
budgeted (theme walks, decodes, messages per dispatch, elements per
`Notify`).

Invariants 3, 4, 6, 7 and 8 live in the structures above (the pipeline
head, the warm/draw gate, the hops, the shutdown gate). No module
interface mutates the window list (1); hyprpm owns deployment, teardown
cancels hops before the buses close, and the build never overwrites a
mapped `.so` (2, 5).

## The look (Frost)

One material, its tokens in `core/tokens.hpp` (the schema's defaults;
the user's `theme.lua` overrides them).

- **Glass**: every surface wears `theme:col_glass`, which carries no
  tint: the material is the user's `decoration:blur`. The fork's
  `highlights` (0.46) rolls luminance off, f(Y) = 0.46 (1 − e^(−Y/0.46)):
  black stays black, white settles at 104/255 so white titles read.
  Cards and menus add a soft drop kept clear of the glass and an 18 %
  white 1 px rim; menus float higher.
- **Text** over glass has no drop; the center's heading, bare on the
  wallpaper, keeps one.
- **Ink**: one white at four strengths (titles and bar 100 %, body 88 %,
  meta 78 %, empty 34 %). The accent `#00CCFF` is text: the viewed tag
  and the launcher's pick (on a flat white 8 % cell, awesome's
  `bg_focus`), the focused task, a selection. The thing acted on now sits
  on a white 16 % pill (radius 6): a hovered menu row, the bell while the
  center is open. The accent fills only what is live (a caret, the send
  circle, progress, the snap preview's 10 % tint) and the bell's dot.
  Urgent is `#FF6159`, as text.
- **Radii**: cards 18, menus 12, rows and pills 6, faces and hero images
  9; tag and launcher cells square; windows keep `decoration:rounding`.
- **Glyphs**: the plugin's outline icons (`core/glyph_shapes.hpp`, each
  parsed by the unit tests: librsvg drops a malformed shape silently),
  white, tinted at raster time, rastered at device size.
- Hover lifts a step; only a menu row takes a ground. Nothing darkens
  the desktop. A **note** (`SPaint::note`: dark 88 % pill, hairline rim,
  drop, no glass) names what the pointer rests on after 500 ms: the
  battery.

## One home per option

Every option has one control; an indicator may echo it, never control
it.

| Option | Its home |
|---|---|
| Do Not Disturb (on, a duration) | the center's moon (`suspend()` for binds) |
| Quiet automatically, silenced apps and chats | the bell's right click |
| An app's banners and sounds, a chat's mute, Close All | the card menu (hold a card) |
| Volume, microphone, brightness, touchpad | their keys (a capsule answers) |

## Module notes

**shell**: tasks are views of `windows` state; minimize, restore and
focus are requests to it. The bar yields to the *presented* fullscreen
(`getPresentedFullscreenModes`): upstream's geometry transactions hold
the old mode until the window's new frame is ready, so the bar neither
vanishes early nor returns late. The bell reads the notify model
directly. The tray installs one async match per signal interface
(never a synchronous AddMatch), fetches one icon per item at a time
(storms fold into one more fetch), and size-checks a menu's `icon-data`
before decoding (256 px a side, 64 per reply).

**windows**: one state machine per window with the told state (xdg
`set_maximized`), compositor-grant adoption and work-area boxes. A press
runs one chain (`windows.cpp`): maximize's `Super` swallow, the snap
commit, the click policy. A closing window's size limits come from
hints cached past the toplevel's death (GTK and Firefox destroy it
before the unmap). The CSD contract (fork-side, upstream's content-frame
model): the window box is the client's `set_window_geometry` rectangle,
its min/max bound that, and the compositor shows exactly that part of
the buffer, cropping the shadow margin; input maps through the same
origin.

**notify**: one card renderer for the banner, the center and a stack
(`notify/stack.cpp` adds the deck and header); the card menu draws last
and hits first.

- Transcripts are fitted from the newest end with the layout the raster
  draws (`fittedTranscript`), memoized, refit in the warm.
- `image-data` pixels are held until first upload; past 32 MiB
  model-wide the oldest keep only an icon's size. `fallback_icon_dir` is
  listed by a helper process (4096 images at most).
- The menu's switches, DND and quiet gate the arrival, the DND resume
  and the re-pop alike.
- The center partitions live cards into ranked and silent (read live)
  and stacks each apart; fold and spread state is per visit.
- Clear All marks cards `cleared` under one timer; its end or the
  center's close sends `NotificationClosed`, and an arrival into a marked
  card unmarks it. Teardown drops the timer after the bus, sending
  nothing.
- Quiet's share is a consumer `outputCopyFBState` reports, rechecked
  each second and cached for the render so warm and draw agree.
- The moon's hit carries the DND state the warm drew.
- Ids 9990–9999 are the plugin's own capsules (9991 touchpad, 9992
  display, 9993 volume, 9995 microphone): no client can pin one or ride
  the band's DND bypass.

**system**: wpctl readbacks reject malformed and non-finite output
(`system/wpctl.hpp`). Touchpad hotplug rides the compositor's device
signals behind a 400 ms settle; a flip writes the per-device config
through `hl.device`, skipped when already in the wanted state
(`hl.device` recompiles the keymap).

## Performance

The compositor thread runs on rtkit's realtime budget (`RLIMIT_RTTIME`:
150 ms soft, 200 ms hard per burst); no plugin burst may approach it.

- **Warms coalesce**: one bar warm per loop turn; a title change warms
  the strip alone, at most every 16 ms; cards warm on their monitor on
  their own changes; the launcher warms its layer. The center measures
  nothing past the fold.
- No bus round-trip on a live path (bell, DND, capsules are in-process)
  and no synchronous AddMatch.
- First uses are paid early: fonts by the preload, icon names from GTK's
  caches, image pixels on the raster worker.
- Layout re-runs each frame on purpose (~90 µs at worst, ~1 % of a
  60 Hz frame): a cached display list would risk stale content across
  every state source.
