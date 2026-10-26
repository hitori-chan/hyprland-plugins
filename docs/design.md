# Design

How `awesome` is built and why. What it does for the user is in the
module docs ([shell](shell.md), [windows](windows.md),
[notify](notify.md), [system](system.md)) and the
[reference](reference.md); the behavioral contract is those docs plus
the gate's `-b all` summary line ([testing](testing.md)). The crash-class
invariants are numbered in [`AGENTS.md`](../AGENTS.md).

One C++26 plugin, built against the exact `hitori-chan/Hyprland` commit
the compositor runs: same process, PluginAPI and fork headers, no FFI.
It replaced eight plugins in 2026-09, cut by domain; the commit history
has the provenance.

**The fork surface it depends on**: `monitor.blockSolitary` (paint over a
fullscreen window), `reservedChanged` (usable-area changes),
`fullWindowFocus` landing the exact window across workspaces,
compositor-minted activation tokens, and the born-fullscreen float's
windowed-size ask. It assumes the fork's compositor fixes: activation as
urgency, the CSD content frame, the true maximize state, popup positions
in window-geometry space, the session-lock grab check.

## Modules

| Module | Owns |
|---|---|
| `core` | the platform (below) |
| `shell` | the bar (tags, tasks, tray, bell, battery, clock) and the launcher |
| `windows` | the window state machine (normal, minimized, maximized: told state, adoption, restore), focus policy, spawn placement, drag snap |
| `notify` | the fd.o daemon, the card model (conversations, grouping, DND, quiet, the capsule band), banners, the center and its control block, inline reply, sound |
| `system` | the lights (logind), volume and microphone (wpctl), the touchpad policy; feedback as notify capsules |

- Modules talk through `core` interfaces and direct C++ calls; there is
  no bus between them. The plugin's D-Bus is external only: the fd.o
  daemon (`notify`), the SNI tray and dbusmenu (`shell`), and system-bus
  clients (logind, BlueZ and iio-sensor-proxy in `system`,
  power-profiles in the battery).
- A module never reaches into another's state. The seams live in `core`:
  `shell` asks `windows` to minimize; `system` posts capsules through the
  notify model API and publishes the control center's values through
  `core/controls.hpp`; persisted state goes through `StateStore`.
- The input order is the registration order in `main.cpp`: `shell` →
  `notify` → `windows` → `system`. The bar claims its strip and menus
  before the cards, the cards before window policy. The gate's pipeline
  battery pins it.
- Pointer and keyboard only: touch is not a goal, and a touch reaches
  whatever native surface lies beneath.

## core

Single-threaded by the fork's guarantee: the compositor's event loop
drives input, dispatch, frame callbacks and rendering. No locks on
plugin state; ordering is program order plus the hop queue. Two workers
are walled off: the store's disk writer (a serialized snapshot through a
mutex, file syscalls only) and the raster worker (bytes in memory only).
Neither reads plugin or compositor state.

### Supervisor

The one `PLUGIN_INIT` / `PLUGIN_EXIT`.

- **Start**: load the state file, start the desktop index and the raster
  worker, init the modules in registration order, then the input and
  render listeners and the font preload. A throw anywhere undoes all of
  it: a throwing start ejects the plugin without `PLUGIN_EXIT`, and
  nothing may be left pointing into the unmapped `.so`.
- **Stop**: the listeners first, then the hops (cancelled; newly armed
  ones are no-ops, invariant 6), each module's persist and the state
  flush (the windows still open), the font preload, module teardowns in
  reverse order (each closes its own bus), then Jobs, the raster worker
  and the desktop index.
- Every state listener is gated on `compositorShuttingDown()`
  (invariant 8).

### Deferral and time

- **Hops** (`CHop`): the plugin's single `doLater` path (invariant 6).
  Every deferred callback captures the session generation; the queue
  drops callbacks from a dead one. Modules have no direct `doLater`.
- **Wall clock** (`core/walltimer.hpp`): a deadline that is a time of day
  (the clock's next minute, a timed DND's end) is an absolute realtime
  timerfd. The event loop's monotonic timers stand still through a
  suspend; this one fires at once on resume and wakes on a clock step.

### Input pipeline

One compositor listener per event class (button, motion, scroll, key).

1. An event upstream already cancelled passes.
2. A session lock or a native input-capture session: every module drops
   its partial input state (swallow masks, held counters, drags, armed
   zones; invariant 7) and the event passes.
3. The chain, in input order, with consume/pass semantics.

Native ownership past that is each module's own recheck before it takes
a press: seat grabs, implicit pointer grabs, and the native hit-test
stack (layer surfaces, popups, IME). A claimed motion still reaches the
later modules, with the claim set, so they let go of hover: motion is
state, not an action.

Swallow masks are per button, not per device, so the head lets a release
through while a press of that button reached the compositor from another
device (a swallowed release there left the button stuck in the app and
the bar deaf). `hyprctl awesome trace` prints who took each of the last
32 events.

### Canvas

A per-monitor scene. Modules contribute layers (the strip, menus, the
card stack, the center, capsules, the snap indicator) with stable
geometry at an explicit level (`eLayerZ`: bar, cards, snap, popups)
whose paint order agrees with the input order: where two overlap, the
one that takes the press is on top.

- One pass element per layer and monitor, scissored to damage.
- **The warm/draw gate** (invariant 4): textures are created on the event
  loop and drawn no earlier than the next frame. Every visible-state
  transition damages the old and new geometry; hover damages but does
  not rewarm.
- A warm can be scoped to one monitor and one layer (a launcher
  keystroke warms the prompt alone). Only a stale rewarm (a draw that
  found a texture missing) forces every layer, at most 8 times in a row,
  so a texture no warm can build never loops the frame. A raster the
  cache refuses is remembered as empty.

### Text

The bar draws labels through the compositor's `renderText`; the cards
shape with their own Pango layouts (the markup whitelist, the
literal-`<`/`&` rescue, link hit-testing by stripped-text byte offset,
line by line). Text that must be one line is one line: the bar replaces
a label's control characters (a newline per paragraph once made a
texture tens of thousands of px tall), the cards use Pango's
single-paragraph mode, and every card raster is clamped to its slot.

Both share one Pango font map and one set of font options
(`core/text.hpp`: full hinting, hinted metrics, whole-pixel glyph
positions); `renderText` sets none, and its fractional placement read
soft beside a terminal at 1x. A face's first layout costs ~10 ms:
`core/fontwarm.hpp` loads the faces the boards use one per event-loop
turn, 1.5 s after start. One shared text engine for both is open work.

### Icons and the desktop index

**One freedesktop resolver** (`core/icons.hpp`) for the bar and the
cards: GTK theme → hicolor → pixmaps, plausible names only, a
two-generation memo.

- A theme dir with a fresh `icon-theme.cache` answers from GTK's own
  index: a hash bucket and a few `pread`s on a held fd (never mapped: a
  cache truncated under a mapping would SIGBUS the compositor), every
  offset bounds-checked. A dir is re-probed every 5 s. The walk it
  replaces cost 35–180 ms for a name the themes lack.
- A dir without a cache is walked, at most 8 walks per 250 ms; past that
  a lookup is deferred and the caller retries a beat later. A config
  reload flushes the memo only on a theme switch.
- **File admission**: a file is opened once, non-blocking, and judged on
  that fd (a regular file under 32 MiB, 2 MiB for an SVG, a bounded
  declared raster). Decoders read the bytes from it, or `/proc/self/fd`
  of the held file for the cards' async decodes (the compositor's
  resource gatherer, 24 slots, one decode shared per file per warm):
  never the path again, so a FIFO or a swap after the check never
  reaches a decoder.
- The bar decodes its icons synchronously but budgeted: 1024 px a side,
  at most two large decodes (past 256² px, or an SVG past 64 KiB) per
  250 ms, all decodes together 4 ms per 16 ms, the rest deferred. A tray
  pixmap is scaled to twice its cell as it arrives.

**The desktop index** (`core/desktopindex.hpp`): one scan of the XDG
application dirs, shared by the bar, the cards' identities and the
launcher. It runs in a bounded, cancellable helper process
(`core/fileindex.hpp`: a plugin cannot leave a filesystem thread alive
across `dlclose`). The event loop drains its framed records with
backpressure; teardown closes the pipe and signals its process group
through a pidfd. A refresh builds aside and swaps in whole; a scan cut
short (its 30 s deadline, a failed helper) swaps nothing and is retried.
The dir stats that decide a refresh still run on the compositor thread
(TODO).

### Configuration and state

- **Config schema** (`core/schema.hpp`): one constexpr table of key,
  module, type, default and range. A getter's key is checked when the
  call compiles (`CfgKey`, consteval), so a typo is a build error, and a
  read is an index. Out-of-range values clamp, non-finite doubles fall
  back to the default, strings are bounded (4 KiB, cut at a code point).
  The defaults are the Frost tokens (`core/tokens.hpp`).
- **Store** (`core/store.hpp`, `StateStore`): the plugin's one persistent
  state, one typed file (rows in the [reference](reference.md#state)).
  One durable atomic write (a private 0600 temp, never through a link,
  `fdatasync`, rename, directory fsync) on a writer thread: the newest
  snapshot wins, and the stop's save joins it. Every row is admitted or
  skipped under fixed bounds; at a bound the least recently remembered
  row goes. An unreadable file blocks writes.
- **Options** (`core/options.hpp`): the choices made from a menu, one
  schema row each (name, default, range), only values off their default
  written. A change marks the store dirty; the saver coalesces per
  event-loop turn. The config files stay read-only to the plugin, and
  nothing is both a config key and an option.
- **Controls** (`core/controls.hpp`): the control center's seam. `system`
  publishes the levels, switches and outputs as it learns them and
  registers the doors that move them; `notify` draws them and sends the
  user's moves through the doors. A changed hook repaints the open
  center; a panel flag tells `system` the center shows a level, so it
  posts no capsule for it.

### Processes and pixels

- **Spawning** (`core/proc.hpp`): `pidfd_spawnp` with an empty signal
  mask and default dispositions, stdin and stderr on /dev/null, every fd
  above 2 closed, the exit status read from the pidfd under the
  compositor's `SA_NOCLDWAIT`.
- **Jobs** run the helpers in class budgets: CONTROL (wpctl: 16, killed
  after 5 s), SOUND (4, the process group killed after 30 s), OPEN
  (xdg-open: 32, never killed). A deadline holds until delivery, so a
  grandchild holding the pipe cannot keep a slot; piped output is capped
  per call; callbacks never unwind into the C dispatch. The launcher's
  `Exec=` goes through the compositor's executor (exec rules, activation
  tokens).
- **Raster worker** (`core/raster.hpp`): one on-demand thread for pixel
  work on bytes already in memory (unpack, scale, crop, tint):
  notification images past 512×512, decimated to within 2x of the card
  first. Work captures only its own buffers; dones run on the event
  loop, are matched to their card by a ticket and upload in the card's
  next warm. A full queue (32 jobs, 64 MiB) refuses and the caller works
  inline; past 4 MP one inline scale runs per 100 ms.

### The notification bus

The fd.o daemon (spec 1.3) stays asynchronous off hot paths (`post()`
for bus-originated work, `pollSoon()` dispatch capped at 64 messages and
4 ms).

- `Notify` is a raw method handler: sdbus-c++'s typed read copies each
  hint Variant into a message of its own (0.2–0.65 s for a screenshot's
  `image-data`). The image header is validated first and its pixels read
  zero-copy, then copied once; only basic-typed hints become Variants (64
  at most); everything else is walked past without a copy.
- sd-bus leaves no container unread and serves no block from a message
  that needs byte-swapping, so a walk is linear in elements and a
  big-endian image is read byte by byte. Past 65,536 walked elements
  (~17 ms) or 256 KiB of a big-endian image (~26 ms), the call is refused
  with a D-Bus error: the defined behavior at a bound only a hostile
  sender reaches.
- The daemon's signals leave in order from one queue; actions, tokens
  and replies go to the sender only. A chat card answers for one id per
  message, because Telegram keeps a record per message and acts on the
  first record holding a signal's id (one shared id made Reply quote the
  chat's oldest message). The queue holds a Clear All of `max_notifs`
  chats with full logs (512 × 33 signals, plus room for the clicks).
- The tray installs one async match per signal interface, routed by
  sender and path, never a per-proxy synchronous AddMatch.

### Bounds

Every externally sized structure has a bound and a defined behavior at
it, enforced where it enters: client-fed queues are `CHopQueue<T, N>` (a
push at the bound is refused), wire strings are cut at a code point
(`clipToUtf8Boundary`, `BoundedString<N>`), images are decimated while
unpacked, raster caches are capped in bytes, and work is bounded too
(theme walks, large decodes, messages per dispatch, elements per
`Notify`). `BoundedQueue<T, N>` carries an eviction policy in its type.

### Invariants → structure

| Invariant | Guarantee |
|---|---|
| 1 window list | no module interface exposes compositor window-list mutation |
| 2 plugin unload | hyprpm owns deployment; teardown cancels hops before the buses close |
| 3 key releases | the pipeline head owns the swallow masks; lock or capture resets all; no release is cancelled while a press of its button that reached the compositor is held |
| 4 texture frame | the canvas enforces the warm/draw gate for every layer |
| 5 mapped `.so` | the build never overwrites it in place |
| 6 deferrals | the single hop queue; teardown cancels and no-ops new arms |
| 7 lock precedence | the pipeline head checks the session lock first, always |
| 8 shutdown | every state listener no-ops once the compositor is shutting down |

## The look (Frost)

One material for every surface, its tokens in `core/tokens.hpp` (the
schema's defaults name them; the user's `theme.lua` overrides them).

- **Glass** at three thicknesses over the compositor's blur: thin for the
  bar and the launcher, regular for cards, the center and capsules, thick
  for menus. Cards and menus add a soft drop, a light top edge and a 10 %
  white rim; a menu floats higher.
- **Ink** is one white at four strengths (titles and the bar 100 %, body
  78 %, meta 55 %, empty 34 %). The accent `#00CCFF` is text: the viewed
  tag, the focused task, a selection, the bell while the center is open.
  It fills only what is live (a caret, the reply's send circle, progress,
  a lit tile; the snap preview is its 10 % tint) and marks one thing, the
  bell's dot. Urgent is `#FF6159`, as text.
- **Radii**: cards 18, menus 12, rows and tags 6, buttons and app icons 9;
  windows keep the user's `decoration:rounding`.
- **Glyphs** are the boards' own outline icons
  (`core/glyph_shapes.hpp`, each parsed by the unit tests: librsvg
  refuses a malformed shape whole and the glyph silently never draws),
  drawn white and tinted at raster time. Icons raster at the monitor's
  device size, never a larger raster shrunk on the GPU.
- **References**: the bar's right end follows macOS 27's menu bar,
  measured off its 2x screenshot; the battery is Pixel's
  (`shell/battery_pill.hpp`, pure cairo); cards follow macOS 27's
  notifications without the hover buttons.
- Hover lifts text a step, never a ground. Nothing darkens the desktop:
  what floats over the wallpaper carries its own glass, or a text
  shadow.

## shell

Tasks are views of `windows` state: minimize, restore and focus are
requests to `windows`, never state kept in the bar. The bell reads the
notify model directly (the dot, DND, the center) and calls notify's
center toggle. Its right-click menu is built locally (the client list's
kind) over notify's own doors: timed DND, quiet automatically, the
silenced apps.

The tray fetches one icon per item at a time (a `NewIcon` storm folds
into one more fetch; `NewIconThemePath` and `NewMenu` fold the same
way), bounds every item string, picks a pixmap for the densest monitor's
scale, and size-checks a menu's inline `icon-data` before decoding
(256 px a side, 64 per reply). The launcher's paste is non-blocking
with a 1.5 s deadline; closing the launcher removes the fd and
invalidates the callback.

## windows

One state machine per window, `normal | minimized | maximized`, with the
told state (xdg `set_maximized`), compositor-grant adoption and
work-area boxes, plus the focus policy and placement. The window-press
arbitration (swallow, focus, or let the compositor's drag proceed) is
one function. A maximized window's press is swallowed whole, with its
release. Spawn spots and windowed boxes persist as `spot` and `windowed`
rows. The size limits of a closing window come from the compositor's
cached copy, since GTK and Firefox destroy the toplevel before the
unmap.

**The CSD contract** (fork-side, matching upstream's content-frame model,
the one its popup math already assumes): the window box is the client's
content frame (its `set_window_geometry` rectangle), and the client's
min/max bound it as-is. The compositor shows exactly that rectangle of
the larger buffer (content plus shadow margin) at the box, cropping the
margin: never padding between the border and the content, never a
blurred halo outside it. Input maps through the same origin, so the
pointer lands on the pixel it is over. (2026-10-04: the surface-frame
model shipped the margin inside the box as blurry padding; 2026-10-05:
with only the drawing cropped, every click on a floating CSD window
landed up-left by the shadow margin.)

## notify

One card renderer for the banner, the center and a card in a stack
(`notify/stack.cpp` draws the deck and the spread stack's header around
it); the card menu is drawn last and hit first on the notify layer.

- **Transcripts**: a chat's window is fitted from its newest end
  (`notify/transcript.hpp`, pure) with the layout its raster draws
  (`fittedTranscript`), so the raster never cuts what the fit kept.
  Counted one paragraph a layout and checked once whole, the exact fit
  only when a taller line fails the check: about a millisecond, memoized
  per revision, width, line budget and font, refit in the warm, never per
  frame.
- **Images**: `image-data` pixels are held until the first upload; past
  32 MiB model-wide (coalesced, resident and DND-held cards) the oldest
  keep an icon's size instead. `fallback_icon_dir` is listed recursively
  by a helper process (4096 images at most).
- **The arrival gate**: the menu's switches (an app's banners and
  sounds, a chat's mute), DND and quiet automatically gate the arrival,
  the DND resume and the re-pop alike.
- **Quiet automatically** (`notify/quiet.cpp`): a share is a consumer the
  compositor's `outputCopyFBState` reports (a frame copied within half a
  second, or one waiting on a still screen), never the session alone,
  which a wlr-screencopy client keeps until it disconnects. Rechecked
  each second while up, and cached for the render so a critical banner's
  hushed layout agrees between warm and draw.
- **The control block** (`notify/controls.cpp`): the warm snapshots
  `core/controls.hpp` and the draw paints the snapshot. A dragged slider
  shows what it asked until the device reports that level (1.5 s at
  most), so a set in flight never snaps it back. A right click opens the
  card menu for that control (`CardMenu::openControl`), rebuilt while
  open so a late read shows; a pick finds its row by key, not index, so
  a rebuild cannot shift it.
- **Capsules**: ids 9990–9999 are the plugin's own in-process posts, so
  no client can pin one, take over the volume card or ride the band's
  DND bypass. Fresh client ids come from a low counter outside it.

## system

- **Lights** (`system/light.cpp`): logind `SetBrightness` for the
  backlight and the keyboard's LED, `/sys` reads, one write in flight
  per light with the presses summed into the next.
- **The keyboard in the dark**: the displays' own `dpmsChanged` (emitted
  inside `setDPMS`, before the commit), checked once a turn out of the
  emission. No enabled monitor is no verdict: losing every output is not
  a wake.
- **The room**: iio-sensor-proxy (`net.hadess.SensorProxy`), claimed
  only while followed, read in lux with a margin (20/60 lux) and a 5 s
  settle. A hand choice holds until the zone changes; the on level is
  the user's last (`kbd-level`, remembered at logind's ack).
- **Audio** (`system/audio.cpp`): one wpctl set per device then a
  readback, the asks accumulating meanwhile; the wheel and the keys
  send relative steps, so a coarse device moves too. Readbacks reject
  malformed and non-finite output; `wpctl status` is parsed for the sinks
  (`system/wpctl.hpp`, bounded: 16 sinks, 96-byte names, 32 KiB of
  output). A headset's own volume (BlueZ's transport `Volume`) reads the
  sink back at most every 120 ms.
- **Touchpad** (`system/pad.cpp`): hotplug rides the compositor's device
  signals behind a 400 ms settle; the switch writes the per-device config
  store through `hl.device`, and a touchpad already in the wanted state
  is left alone (`hl.device` re-runs the device setup, a keymap compile).
- **Feedback**: capsule ids 9991 touchpad, 9992 display, 9993 volume,
  9994 keyboard light, 9995 microphone; low urgency, a glyph, a value.
- **The control center**: every value is published into
  `core/controls.hpp` as it is learned (a readback, logind's ack, a
  touchpad pass); the doors take absolute percents, relative steps and
  switches, deferred out of the input emission, newest ask winning.

## Interfaces

The [reference](reference.md) lists them: one flat Lua namespace
`hl.plugin.awesome.<fn>`, `hyprctl awesome <verb>` routed to the owning
module (`trace` is the supervisor's), the config keys and the state
rows. The pre-monolith names (`hyprctl hyprnotify`, the per-plugin
`hl.plugin.*` functions, `org.hitori.hyprnotify`) are removed, not
aliased. The version is MAJOR.MINOR.PATCH, in lockstep between
`core/version.hpp` and `hyprpm.toml`.

## Performance

The compositor thread runs on a realtime budget (rtkit's
`RLIMIT_RTTIME`: 150 ms soft, 200 ms hard per burst); no plugin burst may
approach it. Measured in the nested gate (2026-10-25, plugin 3.0.1): a
1080p `image-data` answers in 16 ms, 65 `Notify` calls at once peak at
20–52 ms, a session's first card takes 19 ms with Mesa's shader cache
warm (cold, upstream's lazy shader compiles add ~100 ms to its draw).

- One pass element per layer and monitor, each with its own box: the
  renderer culls a layer the damage misses, and an element claims live
  blur only while its layer paints translucent glass.
- **The bar's glass** samples the wallpaper's precomputed blur
  (`ILayer::wallpaperGlass`, blurred once per wallpaper change). A live
  blur re-blurred the strip on every repaint and, under partial damage,
  read the ring each swapchain buffer kept from its last frame, so over
  a terminal taking input the presented frames alternated between two
  bars (upstream's FIXME in `CRenderPass::render`). A frame the
  precomputed blur is stale for falls back to the live one, as does the
  strip over a fullscreen client.
- **Warms coalesce**: one bar warm per event-loop turn; a title change
  warms the strip alone, at most once per 16 ms; the cards warm on their
  monitor and only on their own changes; the launcher warms its layer.
  The center measures no row past the fold.
- Config reads are an index and a typed read: no hash, no allocation.
- No bus round-trips on a live path (the bell, DND, capsules are
  in-process calls) and no synchronous AddMatch anywhere.
- First uses are paid early: font faces by the preload, icon names from
  GTK's caches, image pixels on the raster worker. The plugin's load
  warms the bar once (~62 ms, mostly the CJK fallback for the tags).
- The draw re-runs layout each frame on purpose: the gate's `drawstats`
  measures ~90 µs for a card and ~90 µs for the bar at worst, about 1 %
  of a 60 Hz frame. A cached display list would save ~100 µs at the cost
  of stale-content risk across every state source.
