# Design

How `awesome` is built and why. What it does is in the module docs
([shell](shell.md), [windows](windows.md), [notify](notify.md),
[system](system.md)) and the [reference](reference.md); the behavioral
contract is those docs plus the gate's `-b all` summary line
([testing](testing.md)). The crash-class invariants are numbered in
[`AGENTS.md`](../AGENTS.md).

One C++26 plugin, built against the exact `hitori-chan/Hyprland` commit
the compositor runs: same process, PluginAPI and fork headers, no FFI.

**The fork surface it depends on**: `monitor.blockSolitary` (paint over a
fullscreen window), `reservedChanged` (usable-area changes), the plugin
reserved type (the bar's band), the monitor's fullscreen inset (a
notch's band), `fullWindowFocus` keeping the caller's
reason and surface across workspaces, compositor-minted activation
tokens, and the born-fullscreen float's windowed-size ask. It assumes the
fork's compositor fixes: activation as urgency, the CSD content frame,
the true maximize state, popup positions in window-geometry space, the
session-lock grab check.

## Modules

| Module | Owns |
|---|---|
| `core` | the platform (below) |
| `shell` | the bar (tags, tasks, tray, bell, battery, clock) and the launcher |
| `windows` | the window state machine (normal, minimized, maximized: told state, adoption, restore), focus policy, spawn placement, drag snap |
| `notify` | the fd.o daemon, the card model (conversations, grouping, DND, quiet, the capsule band), banners, the center, inline reply, sound |
| `system` | the backlight (logind), volume and microphone (wpctl), the touchpad policy; feedback as notify capsules |

- Modules talk through `core` interfaces and direct C++ calls; there is
  no bus between them. The plugin's D-Bus is external only: the fd.o
  daemon (`notify`), the SNI tray and dbusmenu (`shell`), and system-bus
  clients (logind and a headset's BlueZ transport in `system`,
  power-profiles in the battery).
- A module never reaches into another's state: it calls the owner's
  API (`shell` asks `windows` to minimize through `Windows::Tasklist`;
  `system` posts capsules through the notify model's `postCard`), or a
  seam in `core` (`shell` publishes the open launcher's strip through
  `core/bands.hpp`; persisted state goes through `StateStore`).
- The input order is the registration order in `main.cpp`: `shell` →
  `notify` → `windows` → `system`. The bar claims its strip and menus
  before the cards, the cards before window policy. The gate's pipeline
  battery pins it.
- Pointer and keyboard only: a touch reaches whatever native surface lies
  beneath.

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
- Every listener registered through `supervisor().listen` is gated on
  `compositorShuttingDown()` (invariant 8).

### Deferral and time

- **Hops** (`CHop`): the plugin's single `doLater` path (invariant 6).
  Every hop registers itself; re-arming one cancels only its own pending
  callback. Teardown resets them all after the listeners are gone, and
  from then on `arm()` is a no-op. `CHopQueue<T, N>` batches pushes into
  one hop and refuses a push at its bound.
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
later modules, with the claim set, so they let go of hover.

A module's swallow masks are per button, not per device, so the head
counts the presses that reached the compositor and lets a release
through while one of its button is held; otherwise the app sees the
button stuck. `hyprctl awesome trace` prints who took each of the last
32 button, scroll and key events.

### Canvas

A per-monitor scene. Modules contribute layers (the strip, menus, the
card stack, the center, capsules, the snap indicator) with stable
geometry at an explicit level (`eLayerZ`: bar, cards, snap, popups)
whose paint order agrees with the input order: where two overlap, the
one that takes the press is on top.

- One pass element per layer and monitor, each with its own box: the
  renderer culls a layer the damage misses and scissors the rest to it,
  and an element claims live blur only while its layer paints
  translucent glass.
- **The warm/draw gate** (invariant 4): textures are created on the event
  loop and drawn no earlier than the next frame. Every visible-state
  transition damages the old and new geometry; hover damages but does
  not rewarm.
- A warm can be scoped to one monitor and one layer (a launcher
  keystroke warms the prompt alone). Only a stale rewarm (a draw that
  found a texture missing) forces every layer, at most 8 times in a row,
  so a texture no warm can build never loops the frame.
- **Glass repaints whole.** A partial repaint of a live-blurred box
  samples the rest of the box from the swapchain buffer's own old glass,
  so the buffers drift apart and the glass shimmers (upstream's FIXME in
  `CRenderPass::render`). Damage within the blur radius of a glass layer
  repaints the whole layer (`Canvas::wholeGlass`, per buffer).

### Text

The bar rasters its labels as the compositor's `renderText` does (the
same extents, ellipsizing and premultiplied ARGB32); the cards shape
with their own Pango layouts (markup, links). Text that must be one line
is one line: the bar replaces a label's control characters,
the cards use Pango's single-paragraph mode, and every card raster is
clamped to its slot.

Both share one Pango font map and one set of font options
(`core/text.hpp`: full hinting, hinted metrics, whole-pixel glyph
positions); `renderText`'s fractional placement reads soft beside a
terminal at 1x. A face's first layout costs ~10 ms, so
`core/fontwarm.hpp` loads the faces in use one per 40 ms tick, from
1.5 s after start.

### Icons and the desktop index

**One freedesktop resolver** (`core/icons.hpp`) for the bar and the
cards: the GTK theme (and its base, for a `-dark`/`-light` variant) →
hicolor → Adwaita → AdwaitaLegacy → the `pixmaps` dirs, plausible names
only, a two-generation memo.

- A theme dir with a fresh `icon-theme.cache` answers from GTK's own
  index: a hash bucket and a few `pread`s on a held fd (never mapped: a
  cache truncated under a mapping would SIGBUS the compositor), every
  offset bounds-checked. A dir is re-probed every 5 s.
- A dir without a cache is walked, at most 8 walks per 250 ms; past that
  a lookup is deferred and the caller retries a beat later. A config
  reload flushes the memo only on a theme switch.
- **File admission**: a file is opened once, non-blocking, and judged on
  that fd (a regular file under 32 MiB, 2 MiB for an SVG, a bounded
  declared raster). Decoders read from that fd, or from `/proc/self/fd`
  of it for the cards' async decodes: never the path again, so a FIFO or
  a swap after the check never reaches a decoder.
- The cards decode on the compositor's resource gatherer, whose one
  worker the compositor's own text jobs share: 24 jobs tracked, two in
  flight, one decode shared per file.
- The bar decodes its icons synchronously but budgeted: 1024 px a side,
  at most two large decodes (past 256² px, or an SVG past 64 KiB) per
  250 ms, all decodes together 4 ms per 16 ms, the rest deferred. A tray
  pixmap is box-averaged to within twice its cell as it arrives, then
  fitted to it.

**The desktop index** (`core/desktopindex.hpp`): one scan of the XDG
application dirs, shared by the bar, the cards' identities and the
launcher. It runs in a bounded, cancellable helper process
(`core/fileindex.hpp`: a plugin cannot leave a filesystem thread alive
across `dlclose`). The event loop drains its framed records with
backpressure; teardown closes the pipe and signals its process group
through a pidfd. A refresh builds aside and swaps in whole; a scan cut
short (its 30 s deadline, a failed helper) swaps nothing and is retried.
The dir stats that decide a refresh run on the compositor thread.

### Configuration and state

- **Config schema** (`core/schema.hpp`): one constexpr table of key,
  type, default and range. A getter's key is checked when the call
  compiles (`CfgKey`, consteval), so a typo is a build error, and a read
  is an index, never a lookup. Out-of-range values clamp,
  non-finite doubles fall back to the default, strings are bounded
  (4 KiB, cut at a code point). The defaults are the Frost tokens
  (`core/tokens.hpp`).
- **Store** (`core/store.hpp`, `StateStore`): the plugin's one persistent
  state, one typed file (its rows, bounds and load rules in the
  [reference](reference.md#state)). One durable atomic write (a private
  0600 temp, never through a link, `fdatasync`, rename, directory fsync)
  on a writer thread: the newest snapshot wins, and the stop's save
  joins it.
- **Options** (`core/options.hpp`): the choices made from a menu, one
  schema row each (name, default, range), only values off their default
  written. A change marks the store dirty; the saver coalesces per
  event-loop turn. The config files stay read-only to the plugin, and
  nothing is both a config key and an option.
- **Bands** (`core/bands.hpp`): what lies across a monitor under the
  bar. `shell` publishes the launcher's strip as it opens and shuts;
  `notify` stacks its cards under it and restacks off the changed hook.

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
  next warm. A full queue (32 jobs) refuses and the caller works inline,
  as does an arrival once 64 MiB of `image-data` waits to unpack; past
  4 MP one inline scale runs per 100 ms.

### The notification bus

The fd.o daemon stays asynchronous off hot paths (`post()` for
bus-originated work, `pollSoon()` dispatch capped at 64 messages and
4 ms).

- `Notify` is a raw method handler: sdbus-c++'s typed read copies each
  hint Variant into a message of its own (0.2–0.65 s for a screenshot's
  `image-data`). The image header is validated first and its pixels read
  zero-copy, then copied once; only basic-typed hints become Variants (64
  at most); everything else is walked past without a copy.
- sd-bus leaves no container unread and serves no block from a message
  that needs byte-swapping, so a walk is linear in elements and a
  big-endian image is read byte by byte. Past 65,536 walked elements
  (~0.2–1 µs each) or 256 KiB of a big-endian image (~26 ms), the call is
  refused with a D-Bus error. Hints past the 64th and action strings past
  the 64th are walked past; the card keeps eight action rows.
- The daemon's signals leave in order from one queue; actions, tokens
  and replies go to the sender only. The queue holds a Clear All of
  `max_notifs` chats with full logs (512 × 33 signals, plus room for the
  clicks).

### Bounds

Every externally sized structure has a bound and a defined behavior at
it, enforced where it enters: client-fed queues are `CHopQueue<T, N>`,
wire strings are cut at a code point (`core/bounded.hpp`'s
`clipToUtf8Boundary`), images are decimated while unpacked, raster
caches are capped in bytes, and work is bounded too (theme walks, large
decodes, messages per dispatch, elements per `Notify`).

### Invariants → structure

Invariants 3, 4, 6, 7 and 8 live in the structures above: the pipeline
head's lock check and passed-press counts, the canvas's warm/draw gate,
the hops, the supervisor's shutdown gate. No module interface
exposes compositor window-list mutation (1); hyprpm owns deployment,
teardown cancels hops before the buses close, and the build never
overwrites a mapped `.so` (2, 5).

## The look (Frost)

One material for every surface, its tokens in `core/tokens.hpp` (the
schema's defaults name them; the user's `theme.lua` overrides them).

- **Glass**: the bar, the launcher, cards, the center, capsules, chips, a
  stack's deck and menus all wear `theme:col_glass`, which carries no
  tint: the material is the user's `decoration:blur`. The fork's
  `highlights` (0.46) rolls the blur's luminance off,
  f(Y) = 0.46 (1 - e^(-Y/0.46)): black stays black, so the bar sinks into
  a dark wallpaper with no grey floor, while white settles at 104/255,
  dark enough for white titles to read. Cards and menus add a soft drop,
  kept clear of the glass so it never darkens what the glass shows, and
  an 18 % white rim, one pixel all round; a menu floats higher.
- **Text** over glass wears no drop. The center's heading, bare on the
  wallpaper, keeps a desktop label's drop.
- **Ink** is one white at four strengths (titles and the bar 100 %, body
  88 %, meta 78 %, empty 34 %). The accent `#00CCFF` is text: the viewed
  tag and the launcher's pick (each on a flat cell, white 8 %, awesome's
  `bg_focus`), the focused task, a selection. What you act on now sits
  on a white pill (16 %, radius 6) in its own ink: a menu's row under the
  pointer, the bell while the center is open. The accent fills only what
  is live (a caret, the reply's send circle, progress; the snap preview
  is its 10 % tint) and marks one thing, the bell's dot. Urgent is
  `#FF6159`, as text.
- **Radii**: cards 18, menus 12, menu rows and pills 6, an app's face and
  a hero image 9; the tag and launcher cells are square; windows keep the
  user's `decoration:rounding`.
- **Glyphs** are the plugin's own outline icons
  (`core/glyph_shapes.hpp`, each parsed by the unit tests: librsvg
  refuses a malformed shape whole and the glyph silently never draws),
  drawn white and tinted at raster time. Icons raster at device size: a
  card's at its monitor's scale, the bar's at the densest monitor's.
- Hover lifts a step (a tag's text, a card's glass); only a menu row
  takes a ground, the pill. Nothing darkens the desktop: what floats
  over the wallpaper carries its own glass, or a text shadow. What the
  pointer rests on may name itself in a **note**
  (`SPaint::note`: a dark pill, 88 %, a hairline rim and a drop, no
  glass) after 500 ms: the battery.

## One home per option

Every option has one control, in one place; an indicator may echo a
state, never control it.

| Option | Its home |
|---|---|
| Do Not Disturb (on, a duration) | the center's moon (`suspend()` for binds) |
| Quiet automatically, silenced apps and chats (restore) | the bell's right click |
| An app's banners and sounds, a chat's mute, Close All | the card menu (hold a card) |
| Volume, the microphone, brightness, the touchpad | their keys (a capsule answers) |

## shell

Tasks are views of `windows` state: minimize, restore and focus are
requests to `windows`, never state kept in the bar. The bell reads the
notify model directly (the dot, DND, the center) and calls notify's
center toggle. Its right-click menu is built locally (the client list's
kind, with header rows and right-hand details) over notify's own doors.

The tray installs one async match per signal interface, routed by
sender and path, never a per-proxy synchronous AddMatch. It fetches one
icon per item at a time (a `NewIcon` storm folds into one more fetch;
`NewIconThemePath` and `NewMenu` fold the same way), bounds the item's
text, picks a pixmap for the densest monitor's scale, and size-checks a
menu's inline `icon-data` before decoding (256 px a side, 64 per
reply). The launcher's paste is non-blocking;
closing the launcher removes the fd and invalidates the callback.

## windows

One state machine per window, `normal | minimized | maximized`, with the
told state (xdg `set_maximized`), compositor-grant adoption and
work-area boxes, plus the focus policy and placement. A press runs one
chain (`windows.cpp`): maximize's swallow (a `Super` press on a
maximized window, with its release, so no drag starts), the snap
commit, the click policy. The size limits of a closing window come from
the backend's hints, cached past the toplevel's death, since GTK and
Firefox destroy the toplevel before the unmap.

**The CSD contract** (fork-side, matching upstream's content-frame model,
the one its popup math already assumes): the window box is the client's
content frame (its `set_window_geometry` rectangle), and the client's
min/max bound it as-is. The compositor shows exactly that rectangle of
the larger buffer (content plus shadow margin) at the box, cropping the
margin: no padding between the border and the content, no blurred halo
outside it. Input maps through the same origin, so the pointer lands on
the pixel it is over.

## notify

One card renderer for the banner, the center and a card in a stack
(`notify/stack.cpp` draws the deck and the spread stack's header around
it); the card menu is drawn last and hit first on the notify layer.

- **Transcripts**: a chat's window is fitted from its newest end
  (`notify/transcript.hpp`, pure) with the layout its raster draws
  (`fittedTranscript`), so the raster never cuts what the fit kept;
  memoized per revision, width, line budget and font, refit in the warm,
  never per frame.
- **Images**: `image-data` pixels are held until the first upload; past
  32 MiB model-wide (coalesced, resident and DND-held cards) the oldest
  keep an icon's size instead. `fallback_icon_dir` is listed recursively
  by a helper process (4096 images at most).
- **The arrival gate**: the menu's switches (an app's banners and
  sounds, a chat's mute), DND and quiet automatically gate the arrival,
  the DND resume and the re-pop alike.
- **The center's ranking** (`notify/center.cpp`): each warm partitions
  the live cards into the ranked list and the silent ones (low urgency,
  `Policy::silenced`, read live) and stacks each part apart, so an app's
  silent card never rides its loud stack. The fold and the spread stacks
  are visit state, dropped on close.
- **The undo window**: Clear All marks its cards `cleared` (off every
  view) under one timer; the timer's end or the center's close sends
  their `NotificationClosed`, and an arrival into a marked card unmarks
  it. Teardown drops the timer after the bus: nothing is sent.
- **Quiet automatically** (`notify/quiet.cpp`): a share is a consumer the
  compositor's `outputCopyFBState` reports, never the session alone,
  which a wlr-screencopy client keeps until it disconnects. Rechecked
  each second while up, and cached for the render so a critical banner's
  hushed layout agrees between warm and draw.
- **The moon** (`notify/center.cpp`): its hit carries the DND state the
  warm drew, so a click acts on what it showed.
- **Capsules**: ids 9990–9999 are the plugin's own in-process posts, so
  no client can pin one, take over the volume card or ride the band's
  DND bypass. Fresh client ids come from a low counter outside it.

## system

- **Audio** (`system/audio.cpp`): readbacks reject malformed and
  non-finite output (`system/wpctl.hpp`).
- **Touchpad** (`system/pad.cpp`): hotplug rides the compositor's device
  signals behind a 400 ms settle; a flip writes the per-device config
  store through `hl.device`, and a touchpad already in the wanted state
  is left alone (`hl.device` re-runs the device setup, a keymap compile).
- **Feedback**: capsule ids 9991 touchpad, 9992 display, 9993 volume,
  9995 microphone; a glyph, a value or words.

## Performance

The compositor thread runs on a realtime budget (rtkit's
`RLIMIT_RTTIME`: 150 ms soft, 200 ms hard per burst); no plugin burst may
approach it. Measured in the nested gate (plugin 3.0.1, Mesa's shader
cache warm): a 1080p `image-data` answers in 16 ms, 65 `Notify` calls at
once peak at 20–52 ms, a session's first card takes 19 ms (cold,
upstream's lazy shader compiles add ~100 ms to its draw).

- **Warms coalesce**: one bar warm per event-loop turn; a title change
  warms the strip alone, at most once per 16 ms; the cards warm on their
  monitor and only on their own changes; the launcher warms its layer.
  The center measures no row past the fold.
- No bus round-trips on a live path (the bell, DND, capsules are
  in-process calls) and no synchronous AddMatch anywhere.
- First uses are paid early: font faces by the preload, icon names from
  GTK's caches, image pixels on the raster worker. The plugin's load
  warms the bar once (~62 ms, mostly the CJK fallback for the tags).
- The draw re-runs layout each frame on purpose: ~90 µs for a card and
  for the bar at worst, about 1 % of a 60 Hz frame. A cached display
  list would save that at the cost of stale-content risk across every
  state source.
