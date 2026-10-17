# awesome

One native C++26 Hyprland plugin for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork
ABI. Five modules, one `PLUGIN_INIT`, one input pipeline, no
inter-module bus:

| module | owns |
|---|---|
| `core` | the platform: supervisor, input pipeline, hop queue, canvas (warm/draw gate), config schema, stores, jobs, fd.o bus client |
| `shell` | the bar: workspaces, tasks (views), tray, bell, battery, clock, menubar launcher |
| `windows` | the window state machine and all placement: maximize/minimize/restore, click/focus policy, spawn placement, drag snap |
| `notify` | the `org.freedesktop.Notifications` daemon: cards, conversations, shade, popups, inline reply, sound |
| `system` | volume/mic (wpctl), brightness (logind), touchpad policy — every action's feedback is a notify card |

The plugin owns `org.freedesktop.Notifications`; disable another
daemon (dunst, mako) before enabling it.

## Behavior

### The bar (shell)

The strip, launcher, and tray menus are compositor-drawn on the focused
monitor. Before intercepting any input the pipeline rechecks session
lock and native input ownership (layers, popups, IME, grabs,
input-capture); native wins and resets the plugin's partial input
state. The pipeline takes pointer and keyboard input only: touch stays
native, so a touch on the bar, a card or a menu reaches whatever
native surface lies beneath it. Geometry and hitboxes are monitor-local; fixed widget slots and
tray clipping protect the task area on narrow outputs. Textures follow
the canvas warm/draw gate: warm on the event loop, draw no earlier than
the next frame, scissor to damage, damage both old and new geometry for
visible transitions.

- **Workspaces** render as tag cells, **tasks** as view cells, both
  pixel-drawn at their own measured widths (nothing fixed-pitch).
- **Tasks**: Hyprland has no plugin-owned minimized state, so the
  windows module removes a minimized window from layout/render
  participation and remembers its fullscreen, maximize, and floating
  state for restoration. Tasks stay in arrival order; focus cycling
  skips minimized windows. A task's minimize/restore repaints through
  the task-changed hook. X11 override-redirect surfaces (menus,
  tooltips, dropdowns) are not clients, as in awesome: never a task, a
  tag occupant, a focus stop, a snap edge or a placement blocker.
- **Tray**: owns the StatusNotifier/dbusmenu connection. A tray click
  is the activation — after the SNI `Activate` call the bar resolves
  the item's bus PID and focuses the app's own topmost window, for
  every backend (core/activate.hpp). The plugin performs the raise
  itself because no backend can be trusted to: an X11 client's
  SetForegroundWindow arrives as an urgency ping (unauthenticated — the
  fork maps it to urgency only), and a Wayland sender may simply never
  spend the token; in vanilla awesome the app activates itself after
  the click, and the plugin makes that unconditional. A sender that DID
  spend the token just lands on its already-focused window.
  Every item's and menu's signals arrive through ONE connection-wide
  match installed asynchronously (a per-item subscription is a
  synchronous round trip on the compositor thread), routed by sender
  and path; an item fetches one icon at a time, a NewIcon storm folding
  into one more fetch. Item strings are bounded, icon names resolve
  through the core resolver (an item's own `IconThemePath` first), and
  a pixmap is picked for the densest monitor's scale. Menus support
  updates, cascades, scrolling, and check/radio state; a level with no
  rows says "(empty)", or "(unavailable)" when its layout call failed,
  instead of the loading "…" forever; an inline
  `icon-data` PNG is size-checked before decoding (256 px a side, 64 per
  menu reply); providers may emit a leading, doubled, or trailing
  separator and the parse trims them. A menu or the prompt on a monitor
  that goes away closes. Keyboard navigation, tooltips, and overlay
  icons are not implemented.
- **Bell**: reads the notify model directly (same process — no bus
  seam). A click toggles the shade; pointer hover does nothing.
- **Battery** watches the udev battery packs (a dual-pack laptop reads as one; a USB-C port's or a headset's battery uevent is ignored); the clock ticks on the wall clock's minute, re-reads the timezone each tick, and a resume or a clock step updates it at once.
- Labels are one line: a client's title or menu label has its control
  characters replaced before it is rastered (a newline per paragraph
  once made a texture tens of thousands of px tall), and a raster the
  cache refuses is remembered as empty rather than rebuilt each frame.
- Task, tray, menu and launcher icons decode on the compositor thread,
  so the work is budgeted: an icon file is held to 1024 px a side, at
  most two large decodes (past 256² px, an SVG past 64 KiB) run per
  250 ms, all decodes together get 4 ms per 16 ms (the rest fill in over
  the next frames), and a lookup a budget refuses is not a miss — it
  stays uncached and a warm a beat later tries again (a menu row keeps
  the name to ask with). A config reload keeps
  the resolved icons unless the GTK icon theme changed; only the misses
  are dropped (an icon installed since then still lands).

### The launcher (menubar)

Desktop discovery and completion run in a bounded cancellable helper
process. The compositor drains framed results through the event loop;
queue backpressure pauses the helper (a frame too large for the queue's
room waits whole, never truncating the scan), and teardown closes the
pipe and signals its private process group through a pidfd, without
joining filesystem work. Symlinked roots are followed. The app index
rescans on the next open once an applications directory changed (an
app installed or removed since). Desktop Entry strings are decoded
before use (space around `=` is ignored, per the spec); `Exec=`
preserves its quoting and field-code grammar before passing argument
boundaries to the executor.

The typed line runs through the executor's `/bin/sh -c`, so a
completed name is inserted as one escaped word (a file named
`x$(cmd)` stays a file name; spaces, quotes and `=` are backslashed),
and a name with a control or bidi character is never offered. An
escaped space keeps a word whole for the next completion. Every launch
carries a compositor-minted activation token (`XDG_ACTIVATION_TOKEN`
and `DESKTOP_STARTUP_ID` in the environment, `activation-token` in a
D-Bus `Activate`'s platform data): the Enter that ran it was swallowed,
so an already-running app could not otherwise raise itself. Held keys
repeat at the keyboard's own delay and rate (the compositor repeats
only its binds). An exclusive-keyboard layer (a launcher or power menu
a Super bind opened over the prompt) closes the prompt instead of
losing its keys to it, and so does the prompt's monitor being disabled
or removed. The prompt shows a window of the query around the cursor.

| Keys | Action |
|---|---|
| `Left/Right`, `C-j/k`, `Home/End` | select |
| `Enter` | run selected entry |
| `C-Return`, `C-M-Return` | run raw query directly or in a terminal |
| `Tab`, `Shift-Tab` | complete command/path |
| `Up/Down`, `C-p/n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | edit |
| `C-v` | paste asynchronously |
| `Escape` or pointer click | close |

Typed and pasted input shares a 4 KiB UTF-8 limit (completions too);
a paste drops control and bidi characters. Clipboard transfer
is nonblocking with a 1.5-second deadline; closing or tearing down the
launcher removes the fd and invalidates the callback. History lives in
the plugin's one state file (see State).

### Windows

- **Maximize** is per-window and does not consume Hyprland's
  fullscreen slot: the window fills the current workarea and carries
  the native xdg maximized state; app- or compositor-maximized windows
  are adopted into the same model, and a client's own unmaximize (its
  CSD titlebar restore button or double-click) leaves it the same way
  `Mod+M` does — the compositor alone drops that request for a window it
  does not hold maximized, which left such clients told maximized and
  reopening maximized. Maximized windows follow workspace, output, and
  reserved-area changes, and the pointer follows every such geometry
  change (a window that grows under a still cursor takes the next click). A maximized window
  does not move, as in awesome's `mouse.client.move`: `Mod+click` drags
  are swallowed, and a drag the client starts from its own titlebar is
  ended at once. Windowed geometry is restored from the store,
  constrained by the current workarea and the client's size hints. A
  grouped window is neither minimized nor plugin-maximized (its layout
  slot is the group's). A plugin reload keeps both states: minimized
  windows are shown again as the plugin stops (nothing else would know
  to), and the next instance adopts the windows still told maximized.
- **Click policy**: a plain left click raises the target; clicking a
  maximized window tucks fullscreen-flagged floaters without rewriting
  z-order. Keyboard focus raises, pointer hover does not. A short-lived
  corpse guard consumes presses aimed at a window that just closed or
  left fullscreen, preventing click-through to the window below.
  `focus_next`/`focus_prev` cycle stable arrival order (not the
  z-order that click-to-raise keeps changing); `focus_prev_here`
  toggles the two most recent windows on the current workspace. Both,
  and the snap magnet, work on an open special workspace when one covers
  the regular one.
- **Focus semantics (attention-first, the shipped default)**: the
  mechanism is awesome's `permissions.activate` (source-verified), and
  the shipped default picks the Windows foreground model — a background
  app's "present me" asks for ATTENTION, never the focus (this is what
  keeps a message arriving in Telegram from stealing the keyboard):
  - A newly mapped window takes initial focus (awesome's global
    `focus = awful.client.focus.filter` rule; the compositor's new-map
    focus, left authoritative) — that is how a spawned terminal gets
    it, and why a tray-returning app's re-map takes focus as in
    awesome.
  - An activation ask (xdg-activation, the token-validated ask) NEVER
    takes the focus with `misc:focus_on_activate` off (the fork
    default, the live config): the asking window's task chip gets the
    urgent tint — a fork extension over upstream Hyprland, which
    ignores such asks silently. A chip click, `Mod+U` (`hl.dsp.focus({
    urgent_or_last = true })` — the urgent window wins over the
    last-window fallback and the view follows it to its workspace,
    awesome's `awful.client.urgent.jumpto`), or a tray/notification
    click completes the attention.
  - A focused, visible window that asks is a no-op, in both gate modes
    (awesome's `permissions.urgent` never marks the focused client;
    marking it would tint the active chip while the user watches it).
  - An ask on a not-visible window (another workspace, or minimized) is
    urgency only: no focus, no workspace switch — awesome's
    `isvisible` branch, performed in the fork's `CWindow::activate`
    (`m_workspace->visible()` is the fork's own idiom for "on a
    monitor"). An ask on a MINIMIZED window tints the chip; with
    `focus_on_activate` off the restore is a user action (chip click,
    `Mod+Ctrl+N`).
  - Focus clears the urgency mark (EWMH / FS#1310; the fork's
    FocusState does it), as in awesome's client focus handler.
  - A FORCED activation — a taskbar's or a window switcher's pick
    (wlr-foreign-toplevel `activate`) — is the user's own and focuses
    wherever the window is, in both gate modes. A taskbar's
    minimize/unminimize (wlr-foreign-toplevel) runs through the same
    minimize state machine as a client's own minimize button.
  - `focus_on_activate = true` (or the window's own rule) switches to
    vanilla awesome / KWin-token behavior: an ask on a VISIBLE window
    focuses it, and a Wayland ask on a MINIMIZED one restores and
    focuses it (awesome's `permissions.activate` un-minimizes; X11 asks
    stay urgency-only, below). An ask on another workspace is still
    urgency only. The user's live config leaves the value at the fork
    default (attention-only).
  - X11 `_NET_ACTIVE_WINDOW` is urgency ONLY in both gate modes (the
    fork's XWM, documented there): X11 cannot authenticate a gesture,
    and Wine/Proton send it on every internal SetForegroundWindow. A
    user-initiated activation is performed by the plugin instead: tray
    and notification clicks focus the app's own window compositor-side
    (core/activate.hpp), for every backend — restoring it first when its
    only window is minimized — also the fallback when a
    Wayland sender never spends the token the plugin minted for it
    (the plugin makes awesome's "the app activates itself after the
    click" unconditional).
- **Spawn placement** is per-class: the last free position from the
  store (relative to the monitor it was on, applied on the monitor the
  window opens on), else least-overlap for a second window of the same
  class (scored against the topmost 24 windows of the workspace).
  Placement only moves a window: its size is always the client's own
  (the initial configure stays 0x0, so apps restore their own sizes and
  dialogs open at their natural size). The one size the plugin supplies
  is the unmaximize restore box of a window born maximized. Client size
  limits, fullscreen/maximize state,
  pending native requests, rules, parent-anchored dialogs, X11
  geometry, and override-redirect surfaces stay authoritative. The close
  is remembered however the client tears down (GTK and Firefox destroy
  their toplevel before the window unmaps): the size limits are read from
  the compositor's cached copy, not the live toplevel. A
  fixed-size toplevel (min == max — a dialog or splash) keeps the
  compositor's native centered placement and never reads or writes the
  class row.
- **Snap**: during a floating move drag, nearby window and workarea
  edges pull together within `snap_distance`; reaching one output edge
  previews a half, two edges that corner's quarter. Release commits the
  preview regardless of whether the pointer button or `Super` is
  released first. Previews honor the client's size limits and keep the
  chosen edge anchored; gaps between outputs do not arm a zone.
  Disable Hyprland's native `general:snap` to avoid competing
  policies.

### Notifications (notify)

Cards render top-right on the focused monitor — the stack follows a
keyboard or workspace focus flip, but not one the pointer itself makes,
so a mouse grazing the neighboring monitor's corner leaves the cards
where they are (drawn and clickable there alike until a deliberate move
carries them). They start `offset_y` below the monitor top, never above
the workarea, so a taller strip never runs under them. An open tray menu
or the menubar paints over the cards, as it takes the press first.
Newest at the top, glass·ink skin (frosted graphite, superellipse
corners).

**Spec surface.** Methods `Notify`, `CloseNotification`,
`GetCapabilities`, `GetServerInformation` (spec 1.3); signals
`NotificationClosed`, `ActionInvoked`, `ActivationToken`,
`NotificationReplied`. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`,
`icon-static`, `inline-reply`, `persistence`, `sound`.

All signals leave in the order the cards produced them (a card's close
never overtakes its action). `ActionInvoked`, `ActivationToken` and
`NotificationReplied` go to the notification's SENDER only, never
broadcast: what the user types into a reply, and a token that can take
the focus, are nobody else's on the session bus. `NotificationClosed`
stays a broadcast.

- `replaces_id` updates a card in place, keeping its stack slot; an
  unknown id creates the card under that id. A card belongs to the bus
  name that created it (its replies, actions and token go there): another
  client's `replaces_id`, or a conversation merge from it, starts a fresh
  card instead, as Plasma does. A card whose sender left the bus is
  ownerless, and the app's next instance continues it (meanwhile its
  reply field, buttons and primary are gone: they would reach nobody).
  The 9990s are the private OSD band: its ids, live or
  not, are the plugin's own in-process posts (the system module's OSD
  cards), so no client can pin a band id, take over the volume card on
  screen, or ride the band's DND bypass. Fresh ids outside the band are
  minted from a low counter.
- `expire_timeout`: 0 → sticky, >0 → ms. −1 (server decides) → normal
  and critical cards are sticky until dismissed — a message waits to be
  read; self-declared ephemerals (low urgency, `transient`, `value`
  cards) run `timeout_low`. `timeout_normal` > 0 restores a clock for
  the rest.
- Hints honored: `urgency`; `value` (0–100) draws the progress bar (the
  volume/brightness OSD); `image-data`/`image_data`/`icon_data` raw
  pixmaps; `image-path`/`image_path`; `desktop-entry`; `action-icons`;
  `resident`; `transient`; `sound-file`/`sound-name`/`suppress-sound`;
  `x-canonical-append`; `x-notify-group-key`. A boolean hint is read
  the way clients send it: a bool, an int or a byte (`notify-send -h
  int:transient:1`), or a string.
- Structured-conversation hints (neither name form sits in the
  published spec's hint table; the plain name is tried first, the
  `x-notify-*` alias second — the message-time alias alone is
  `x-notify-message-timestamp`): `conversation-id` (the merge
  contract), `conversation-title`, `conversation-kind`
  (`one-to-one`/`group`/`call`),
  `sender-id`/`sender-name`/`sender-icon`, `message-id`,
  `message-time` (int64, ms epoch), `message-historic` (bool),
  `unread-count` (uint, capped at 999).

**Conversations.**

- A `conversation-id` in the hints is the merge contract: every
  message of that chat is ONE card however many `Notify` calls arrive
  (a replace with the same id joins; a replace carrying a DIFFERENT
  conversation-id starts the other chat's card; a replace with none
  keeps the target's conversation for the same app). Display text and
  category alone are not a contract — two chats can share a title. The
  id is opaque and rejected above 512 bytes (clipping it would merge
  two different chats).
- When a conversation-id is in force, the card's body is the
  TRANSCRIPT: the latest kept messages, CHRONOLOGICAL — the oldest line
  leads and the newest ends the body, so an arrival lands at the bottom
  and pushes the oldest out of the top of the window. The banner
  previews the five newest messages; the shade renders the full seven
  (32 retained). Group senders are prefixed by name, all under an 8 KB
  cap that evicts from the oldest end. `message-id` upserts (an edit
  replaces in place), `message-time` orders, `message-historic` marks a
  backfill (it does not grow the unread count). `unread-count` shows
  the header pill; the header also shows a
  facepile of the distinct senders of the kept messages (max three)
  for `group` chats, each avatar its `sender-icon` or a deterministic
  initials face.
- Without a conversation-id the legacy merge stands: a fresh `Notify`
  whose app + summary matches a live card is joined onto it, bodies
  joined chronologically (newest appended at the end) under the same
  cap — triggered by the fd.o
  conversation categories (`im.*`/`call.*`, where the summary is the
  sender or the room) or `x-canonical-append`. Cards that vanish on
  expiry never merge.
- `x-notify-group-key` (opaque, ≤512 bytes) sub-keys the app's bundle:
  four or more cards of one app share ONE digest per declared group,
  and the digest, the fold and the dismissal all act on that group
  alone. A replace without the hint keeps the group for the same app.

**Markup.** Body and title render the whitelisted Pango subset
`<b> <i> <u> <span> <br>`, a `<span>` keeping only its color,
weight, style, underline and strikethrough attributes (a font, face,
size or lang per span is a fontconfig match per value on the
compositor thread); other tags are dropped and a stray `<`/`&`
that forms no tag or entity survives as literal text, so a
markup-aware sender and a naive one both come out right. Malformed
markup falls back to plain text. `<a href>` in the body is a
hyperlink (rewritten to a styled span and hit-tested by its
stripped-text byte offset) when it is an `http(s)://` or `mailto:` link
— any other scheme stays plain text, since any sender writes these
bodies; a click opens the URL via `xdg-open` and
leaves the card up — but not the shade, since a browser is about to
cover it. The pointer shows the hand over a link. `<img src>` in the
body renders as a thumbnail row below the text; a thumbnail that fails
to load keeps its `alt` text as a one-line body entry.

**Images and icons.** Precedence: `image-data` beats `image-path`
beats `app_icon` beats `desktop-entry`. Each of `app_icon` /
`image-path` / `desktop-entry` may be a file path (a `file://` URI too,
percent-decoded, `localhost` allowed, any query or fragment dropped)
OR a freedesktop icon name, resolved against the GTK icon theme, then
hicolor, then `/usr/share/pixmaps` — the one resolver the bar uses too
(core/icons.hpp): plausible names only, a two-generation memo, and at
most 8 uncached theme walks per 250 ms (a burst of distinct names from a
sender resolves to nothing past that, uncached, instead of stalling the
compositor; the card keeps the name and resolves it again a window
later, its sender faces and action icons too). A `desktop-entry` hint additionally
resolves the .desktop file's own `Icon=` (all `$XDG_DATA_DIRS/
applications` entries are indexed once at plugin start by a bounded
helper process, keyed by the file name and `StartupWMClass`), which
beats an icon-name collision; while the index has not reached the
entry, the icon-name stand-in draws and the index upgrades the live
card when it lands. Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP + SVG
(no GIF; AVIF and JXL files are refused — the admission reads a
bounded raster size from the header, and those headers do not carry
one it parses); big images downscale once at load, not per frame.
`image-data` is box-decimated while it is unpacked, to the hero cap
(past 512×512 on the plugin's raster worker thread, not in the
Notify dispatch; up to 64 MiB of payloads wait for it, past that an
arrival unpacks inline). A card holds those pixels until its first
upload, and past 32 MiB of
them model-wide (coalesced, resident, DND-held cards) the oldest
keep an icon's size instead. A
file is opened ONCE, non-blocking, and judged on that open descriptor
(a regular file, bounded size, a bounded declared raster); the decoder
reads the same file (/proc/self/fd), never the path again, so a FIFO
or a swap after the check cannot reach it. File icons decode off the
render thread on the compositor's async resource gatherer (a bounded
share of its queue, one decode shared by every card asking for the
same file; a sender's SVG too, rastered at the hero width when it
may be one, and a symbolic icon tinted on a copy): a card whose icon is
still decoding keeps the icon column reserved — the text does not
re-flow when it lands. A decode lands full size and is scaled once,
box-averaged first to within 2x of the card: past 512×512 on the
raster worker (the compositor thread only uploads the result). When
the worker's queue (32 jobs) is full the scale runs inline, and past
4 MP one such scale runs per 100 ms and the rest wait for a later
warm. Screenshot-sized
images (256 px in both dimensions) render popup-width as a
cover-cropped hero in any capture shape; the shade keeps its rows
compact and shows the same image as a square-cropped thumbnail in the
icon column (AOSP's collapsed BigPicture). Iconless cards draw a random
face from `fallback_icon_dir` (listed recursively by a helper process,
up to 4096 images; until the listing lands they wear the generic mark),
and when that dir is empty (or a face fails to load) a deterministic
generic application mark, so no card is faceless. The icon column is Android's conversation container: the
CONTENT image leads as the avatar (a true circle for a conversation, a
squircle otherwise) and the IDENTITY (`app_icon`/`desktop-entry`)
rides its bottom-right corner as a badge sized by AOSP's ratios off the
40 dp avatar — a 20 dp badge of which 16 dp is the app glyph and 2 dp
on each side is the rim, so only the rim protrudes. A card with no
content image leads with its identity and wears no badge. One column
says both who sent it and which app carried it.

**Actions.** Non-`default` actions render as a clickable button row; a
left click emits `ActionInvoked` and dismisses the card unless the
`resident` hint holds it. Under the `action-icons` hint each action id
is a freedesktop icon name drawn on the button. The `default` action
(and a lone action) fire on a body left click, on BOTH surfaces, and
are never drawn as a button. `ActivationToken` precedes each invoke (a
compositor-minted xdg-activation token, spec 1.3 ordering); the invoke
also focuses the sender's own window itself, for every backend — a
Wayland sender that spends the token lands on its already-focused
window (vanilla), a Wayland sender that never does, and any X11 sender
(whose own activation is unauthenticated and maps to urgency), still
get the window the click meant (the focus contract, README focus
semantics).

**Behavior.**

- Popup clicks: left invokes the action / opens the link / fires the
  default, then dismisses; right dismisses; middle parks the stack into
  the shade. The cards own the pointer over them — hover never leaks to
  the window beneath.
- One popup per app (`coalesce_popups`): while an app's banner runs
  its clock, the app's next non-critical card lands in the shade
  silently (badge-counted, no sound). A banner with no clock (sticky,
  critical) is no cooldown, and two chats are two cards: each alerts.
- Popup hover HOLDS the timeout: the card under the pointer stops
  counting down, and leaving restarts its full clock rather than
  resuming the sliver that was left. Only one card can be held, because
  only one can be hovered.
- Shade clicks: a row behaves as its banner did — left on the body
  fires the card's primary and dismisses unless `resident`, a link
  opens, a button acts. Rows open by default, so the click is spent
  acting rather than revealing; the CHEVRON is the only fold target.
  Right dismisses; middle is "Clear all". On an app bundle left
  expands and right (or the header ✕) dismisses the whole app; an
  expanded bundle taller than the page shows the children that fit and
  a "+N more" line, and dismissing a child brings the next one in. The
  paging chips (⌃ above the fold, ⌄ N below it) are their own targets:
  a click pages a screenful, like the wheel.
- `max_height` bounds a popup by priority: header, title, one body
  line, progress and the action buttons first, then the images, then
  their alt text; the body takes the rest. A block that doesn't fit is
  dropped whole, never painted past the card. A wrapped body link is
  hit on exactly the text it covers, line by line.
- Acting CLOSES the shade: firing a card's primary, pressing one of its
  buttons or opening a body link all raise something over the panel
  the click was made in, so the panel gets out of the way. fd.o has no
  `isActivity`, and `resident` is the nearest thing it does have, so
  the shade goes exactly when the card goes. Everything that keeps you
  here keeps the shade: a dismissal, a fold, DND, "Clear all", the
  reply field, and a card with no action to fire.
- The close returns the absorbed stack: opening the shade (and a
  popup's middle-click park) stands the live banners down into parked
  shade rows. An EXPLICIT close — an outside click, Esc, the toggle —
  pops those cards back to banners, one per app under the same cap the
  DND resume applies, on fresh timeouts; a close that leaves the
  notifications invisible is a lost-notification bug. An
  action-driven close (the sender is coming up over the parked stack)
  does not re-pop.
- Inline reply (KDE's protocol, which Telegram Desktop speaks): an
  action keyed `inline-reply` is not a button — it grows a reply field
  in the open shade row, and sending emits `NotificationReplied(id,
  text)` and closes the card unless `resident`.
  `x-kde-reply-placeholder-text` and `x-kde-reply-submit-button-text`
  are honored. The field takes the whole keyboard while armed (there is
  no focus to give it); editing is append-and-backspace plus C-u / C-w,
  and a text wider than the field shows its newest end. The armed row
  stays open and on the page; folding it closes the field. A card that
  dies mid-reply (the sender closed it) disarms the field: the key that
  finds it gone is swallowed and the selection dropped, so no keystroke
  reaches another card. Banners have no field.
- Shade keys, while it is open and only then: Esc closes, ↑/↓ move a
  selection (an accent hairline; the page follows it, however tall the
  next row), Space folds, Enter fires the primary, Tab arms the selected
  card's reply field, Delete dismisses. Paging with the wheel or the
  chips drops the selection (Enter and Delete never act off screen).
  Modified chords pass through as user binds, and so does any key with
  nothing to act on.
- Swipe: a horizontal wheel on a row, away to dismiss. It goes through
  the click queue rather than acting in the emission (crash class 6) —
  a swipe is an alias for a click that already exists.
- Bell: a click speaks Toggle (opening absorbs the banners into parked
  rows, a later explicit close re-pops them) and opens the shade on the
  clicked bar's monitor; the badge is part of its target. Pointer hover
  is a no-op.
- Over fullscreen: banners show over a real fullscreen window too — the
  ecosystem default, no quiet-while-fullscreen policy. While a card is
  up over a fullscreen window, the plugin blocks the monitor's solitary
  render and direct scanout through the compositor's
  `monitor.blockSolitary` hook, so the card composites over it; scanout
  re-engages once the last card clears. The open menubar does the same.
- Critical: urgent-colored frame and progress fill, never expires.
- Sound: `sound-file`/`sound-name` play through a player
  (`sound_command`, empty disables); `suppress-sound` mutes one
  arrival. The compositor has no audio backend, so this shells out,
  reaped off the event loop: at most 4 players at once (each one's
  process group killed after 30 s, so a wrapper script's player goes
  with it), a budget of their own — a sound flood never takes the
  volume keys' helpers.
- DND (`hl.plugin.awesome.suspend()`): arrivals collect
  silently with timeouts held, and resume renders the queue newest-first
  on fresh timeouts. An update or a conversation message onto a card
  already shown (a banner, or a row in the shade) updates it in place,
  silently, as it was.
- Residency (`persistence`): an expired banner RETREATS into the shade
  rather than closing, and waits there until dismissed or acted on —
  the shade is the safety net. There is no history and no recall: a
  dismissed card is gone.
- A `transient` card (the spec's "bypass persistence": a screenshot
  tool's preview, a client's own OSD) is feedback, never a
  notification: it shows as a popup while it lasts, then vanishes —
  no shade row, no residency, no badge. Opening the shade (or a
  popup's middle-click park) ends a live one. A critical transient
  card is an ordinary alert. `hyprctl awesome count` answers the live
  total (the lockscreen bell reads it).
- Session lock: cards never render above the lockscreen. Input
  listeners guard-and-reset first; whatever survives the lock repaints
  at unlock.
- Overflow: `max_notifs` caps the model — overflow evicts the oldest
  short-lived card (transient, progress, OSD), else the oldest
  non-critical one (critical last), with `NotificationClosed`.

**Limitations.** GIF images don't decode (hyprgraphics has no GIF
codec). No animated icons (`icon-multi`). Icon-theme resolution is a
pragmatic scan (GTK theme → hicolor → pixmaps), not a full
`index.theme` inheritance engine.

### System

- **Brightness** reads `/sys/class/backlight` (several devices: the
  firmware one, then platform, then raw — the systemd rule), applies 5%
  linear steps with a raw floor of 2, and writes through logind
  `Session.SetBrightness` on the system bus — no root, no udev rule.
  One write is in flight at a time; presses meanwhile add up into the
  next. A device that is missing or stops reading is looked up again on
  the next press (a late driver, a GPU switch); with none, or no system
  bus, the press answers with an "unavailable" card.
- **Volume and microphone**: `wpctl` reads the current value and writes
  the new one inside its own process, so two sets at once lose a step
  (or fold two mute toggles into one). Each device runs ONE set at a
  time, then a readback for the card; presses while it runs accumulate
  (steps sum into the next set, toggles fold to their parity; a set
  the runner refuses keeps them pending). Raising stops at 100%, but a
  volume already past it (set elsewhere) is never pulled down: a raise
  reads the level first unless a readback from the last second has it
  (another mixer, a new default device), and the card shows the true
  percent past 100% over a full bar. PipeWire
  process I/O stays off render and input callbacks (the core Jobs pool:
  pidfd-watched, capped, generation-checked); readback rejects
  malformed or non-finite output.
- **Touchpad policy**: the touchpad turns off while an external
  (USB/Bluetooth) mouse is present and back on when it's unplugged;
  `XF86TouchpadToggle` flips it by hand, and the flip holds until a
  pointer is plugged or unplugged (a config save re-applies it). A
  USB/Bluetooth pointer counts as a mouse once it moves or clicks: a
  receiver with only a keyboard paired, a gaming keyboard's mouse
  interface or a presenter never does. With an external trackpad too,
  the policy drives the built-in one. Hotplug rides the
  compositor's own device signals (a settle timer coalesces the
  burst); the flip writes the compositor's per-device config store
  through `hl.device`, so nothing fights the next config re-apply, and
  a reload re-checks — silently: a card answers only a change the user
  can see, or the manual flip. A touchpad already in the wanted state is
  left alone (`hl.device` re-runs the device setup, a keymap compile).
- Every action's feedback is a card posted straight into the notify
  model: fixed ids (pad 9991, brightness 9992, volume 9993, mic 9995)
  replace in place, low urgency, 1200 ms expiry, a `value` bar, and the
  matching identity icon. Such an OSD card is feedback, never a
  notification: no shade row, no history, no badge; opening the shade
  drops a live one, and a press while the shade is open shows none (the
  shade replaces the popup column it would show on). A critical battery
  alert, posted in the same band, is an alert: it waits under the shade
  like any banner.

## Lua API

Flat under one namespace — `hl.plugin.awesome.<fn>`:

```lua
hl.plugin.awesome.menubar()                -- the launcher
hl.plugin.awesome.maximize()               -- focused window
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

## hyprctl

`hyprctl awesome <verb>` routes to the owning module: notify serves
`count`, `center`, `state`, `badge`, `topline`, `clear`; system serves
`pad`. `trace` (the input pipeline) prints the last 32 button, axis and
key events and who took each — a module, `blocked` (session lock or
input capture), `upstream` (cancelled before the plugin) or `-` (passed
to the compositor): the first stop when a click goes missing.

## Configuration

47 keys under `plugin:awesome:<module>:<key>` — shell 20, notify 24,
windows 3. The table (kinds, defaults, clamps) is
[`core/schema.hpp`](core/schema.hpp); the defaults are the glass·ink
theme tokens, so an empty config still looks right.

| module | keys |
|---|---|
| shell | `height`, `font_size`, `tray_spacing`, `font`, `terminal`, `col_bg`, `col_fg`, `col_focus`, `col_active`, `col_active_bg`, `col_on_active`, `col_empty`, `col_urgent`, `col_urgent_bg`, `col_square_sel`, `col_square_unsel`, `col_frame`, `col_charging`, `col_low`, `col_powersave` |
| notify | `font`, `font_size`, `width`, `max_height`, `max_icon`, `margin`, `offset_y`, `timeout_low`, `timeout_normal`, `coalesce_popups`, `max_notifs`, `ignore_dbusclose`, `rounding`, `rounding_power`, `sound_command`, `fallback_icon_dir`, `col_bg`, `col_fg`, `col_title`, `col_kicker`, `col_frame`, `col_urgent`, `col_highlight`, `col_link` |
| windows | `edge`, `snap_distance`, `col_frame` |

Colors and fonts arrive from the user's `theme.lua`; the C++ defaults
mirror it.

## State

One file: `$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv` — typed
tab-separated rows, the key last:

| row | meaning |
|---|---|
| `spot <x> <y> <class>` | where the app's last window closed |
| `windowed <x> <y> <w> <h> <class>` | its last windowed box: the restore of a window born maximized |
| `launches <count> <name>` | the launcher's ranking, by .desktop Name |
| `history <query>` | the prompt history, newest last |

Positions are relative to the origin of the monitor the window was on: a
spot remembered on one monitor lands at the same place on whichever
monitor the app opens on next. Each kind is kept oldest-first and bounded
(1024 keys, 50 queries): at the bound the least recently remembered row
goes, never a new one. The windows still open when the plugin stops (a
logout) are recorded as closing. One atomic write (temp + rename),
synced to disk on a writer thread so a slow disk never stalls the
compositor; the save at stop is on disk before the plugin unloads. Every
row is admitted or skipped (a hostile file can never take the session
down); a file that exists but cannot be read blocks every write (a
notification says so) — the user's memory is never replaced by a session
that could not see it. Modules reach the stores through the core's
`StateStore` (the documented cross-module seam).

## Build and test

```sh
make -C awesome            # the release plugin (needs the fork headers)
make -C awesome GATE=1     # awesome-gate.so: the gate's variant (test seams)
make -C awesome test       # headless core harness, no fork headers
make -C awesome gate ARGS="-s ~/repo/Hyprland -b quick|all"
```

Before a release, run the gate (`-b all`) and require its final
`ALL CHECKS PASSED` line. The gate is the plugin's behavioral contract:
scenario batteries (shell/windows/notify/system/pipeline/lifecycle, and
`quick`, a smoke of each module, by default) against a throwaway nested
compositor; the input
fixtures and fakes it drives (vptr, vkbd, cliphold, wpctl, logind, …) are
the shared `devtools/`. `-s` stages the fork build's headers and defaults
the compositor to that build. For one-off diagnosis, `gate/probe.sh` runs
the same preflight and then executes a probe body script against the
nested (harness + lib already sourced; teardown always runs).

The nested is a real second compositor inside a private headless host
(labwc — install it once), with its own session and system buses — your
session is never touched: no output, window, focus, cursor, brightness or
volume change, verified by the gate's isolation check. The harness
scripts are in `gate/` and run from a clone as-is:

```sh
bash awesome/gate/launch.sh        # start the host + nested (the gate uses the same)
bash awesome/gate/shot.sh out.png  # screenshot it + a 2x bar crop
bash awesome/gate/dev.sh           # stop + build + launch + probe + shot
bash awesome/gate/stop.sh          # tear down the nested, its host and buses
```

Runtime state (signature, socket, log, gate scratch) goes to
`$HYPR_HARNESS`, default `~/.local/share/hypr-nested` — the repo stays
free of state. Never rebuild the plugin while a nested has its `.so`
mapped (`dev.sh` stops first).
