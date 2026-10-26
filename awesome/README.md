# awesome

One native C++26 Hyprland plugin for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork
ABI: the bar (`shell`), window policy (`windows`), the notification
daemon (`notify`) and the system controls (`system`) over one platform
(`core`). This file is the behavior reference — what the plugin does,
its Lua API, `hyprctl` verbs, configuration and state file; the
architecture and the reasons live in
[`docs/awesome-design.md`](../docs/awesome-design.md).

The plugin owns `org.freedesktop.Notifications`; disable another
daemon (dunst, mako) before enabling it.

Input reaches the plugin in one order: the bar (its strip, launcher
and menus) first, then the cards, then window policy, then the system
controls — so an open menu takes a press before the card under it,
and a card before the window beneath. Before intercepting anything the
plugin yields to native ownership: the session lock and an
input-capture session first, then layer surfaces, popups, IME, seat and
implicit grabs; native wins and the plugin drops its partial input
state. Pointer and keyboard only (touch is not a goal): a touch on the
bar, a card or a menu reaches whatever native surface lies beneath it.

## Behavior

### The bar (shell)

The strip is compositor-drawn on every monitor, showing that monitor's
tags and tasks; the launcher opens on the focused monitor, a tray or
client-list menu on the bar it was opened from. Geometry and hitboxes
are monitor-local; fixed widget slots and tray clipping protect the
task area on narrow outputs.

- The strip is 28 px of thin glass in the bar's face (Fira Code, the
  kanji in IBM Plex Sans JP): the tags at its left, the tasks splitting
  the middle 14 px off either side, and at its right, 20 apart as macOS
  spaces its menu extras, the tray, the battery, the bell and the clock.
  The slot at each end owns its screen edge — the top-left pixel is tag
  一, the top-right the clock — and the right slot's cells meet across
  their gaps, so no click on the strip lands between two targets. Every
  widget owns its cell, one with nothing to click (the battery, the
  clock) included: a click there does nothing, never its neighbour's.
  As macOS's menu bar, the glass frosts the desktop picture alone: the
  work area starts below it, so it blurs the wallpaper (that blur
  computed once per wallpaper change), never the shadow of the window
  beneath, and holds still while that window repaints.
- **Workspaces** are the nine kanji tags in 26 x 22 cells: the viewed
  one accent text, occupied ones the bar's text, empty ones dim, an
  urgent one red text — never a ground, and no occupancy marks: the
  text's strength says it. A hovered tag's text lifts, the viewed one's
  accent toward white. awesome's
  taglist buttons: a click views the tag (creating it on that monitor;
  a tag another monitor owns is left alone), `Mod`+click sends the
  focused window there without following, and the wheel steps through
  this monitor's tags, wrapping.
- **Tasks**: a 16 px icon and the title, ⌃ for a pinned window and +
  for a maximized one before it, in arrival order. The focused task is
  accent text, an urgent one red text, a minimized one dim with its icon
  at half strength — never a fill; a hovered task's text lifts, the
  focused one's accent toward white.
  awesome's tasklist
  buttons: a click on the focused task minimizes it, on a minimized one
  restores it, on any other raises and focuses it; the wheel cycles the
  focus through this workspace's tasks (minimized ones are listed, not
  walked). Right-click for awesome's client list: every window, any
  tag, each row its icon and title read as its task, its workspace's
  tag in a right-hand column; a click goes to it (a minimized one is
  restored). X11 override-redirect surfaces (menus, tooltips,
  dropdowns) are not clients, as in awesome: never a task, a tag
  occupant, a focus stop, a snap edge or a placement blocker.
- **Tray**: 16 px icons in their own colours, `tray_spacing` apart, each
  rastered at the monitor's own pixel size (a theme's 16 px drawing where
  it has one, an SVG at that size, a pixmap box-filtered down to it):
  never a larger raster shrunk on the GPU. It owns the
  StatusNotifier/dbusmenu connection. A tray click is the activation —
  after the SNI `Activate` call the bar resolves the item's bus PID and
  focuses the app's own topmost window, for
  every backend (core/activate.hpp). The plugin performs the raise
  itself because no backend can be trusted to: an X11 client's
  SetForegroundWindow arrives as an urgency ping (unauthenticated — the
  fork maps it to urgency only), and a Wayland sender may simply never
  spend the token; in vanilla awesome the app activates itself after
  the click, and the plugin makes that unconditional. A sender that DID
  spend the token just lands on its already-focused window. A middle
  click is the item's `SecondaryActivate`. An item fetches one icon at
  a time, a NewIcon storm folding into one more fetch
  (`NewIconThemePath` and `NewMenu` fold the same way). Item strings
  are bounded, icon names resolve
  through the core resolver (an item's own `IconThemePath` first,
  re-read on `NewIconThemePath`), and a pixmap is picked for the
  densest monitor's scale. A `NeedsAttention` item shows its attention
  icon, or its plain one when it offers none. The right button (the
  left too for an `ItemIsMenu` item) opens the item's dbusmenu; with no
  menu path the path is read again (`NewMenu` re-reads it as well, and
  an app may export its menu late) before the item gets `ContextMenu`
  — for that click only: not after another click, with a menu open
  since, or more than a second after it.
  A foreground colour change re-tints the cells at once. Menus are the
  card menu's thick glass (`col_menu_bg`): 28 px rows, the hovered row
  in accent text with no fill, hairline separators, a check glyph or a
  dot for check/radio state. They support
  updates, cascades, scrolling and that state; a live update
  keeps the hovered row (by its dbusmenu id), then the pointer settles
  it (a re-sorted list moves the row away from a resting pointer), and
  a submenu's chevron has a
  column of its own, never ellipsized away; a level with no
  rows says "Empty", or "Unavailable" when its layout call failed,
  instead of the loading "…" forever; an inline
  `icon-data` PNG is size-checked before decoding (256 px a side, 64 per
  menu reply); providers may emit a leading, doubled, or trailing
  separator and the parse trims them. A menu or the prompt on a monitor
  that goes away closes. Esc closes an open tray, client-list or bell menu
  (its press only; the release passes; a modified Esc is a user bind
  passing through); other keyboard navigation,
  tooltips, and overlay icons are not implemented. A hover warms the
  menu's own layer alone, and the menu paints in the panels' box, not
  the whole monitor.
- **Bell**: SF Symbols' bell.badge, no count — a blue dot (`col_dot`, the accent)
  on the bell's shoulder while the center holds a card it has not shown
  since the card arrived or changed. Opening the center (and closing
  it) marks what it lists as seen; the DND queue, which it does not
  list, stays unseen. Under Do Not Disturb the bell is the moon, dotted
  while cards wait in the queue; while the center is open the bell is
  lit in the accent and wears no dot. It reads the notify model
  directly (same process — no bus seam). A click toggles the center on
  the clicked bar's monitor: opening absorbs the live banners into
  parked rows, a later explicit close re-pops them. A right click opens
  the notification options, built as they stand: Do Not Disturb ▸ (For
  30 Minutes, For 1 Hour, For 3 Hours, Until Turned Off; while on, the
  row says until when and Turn Off leads), Quiet Automatically ▸ (When an
  App Is Fullscreen, While Sharing the Screen; both off until checked)
  and Silenced Apps ▸, every app switch and chat mute the card menus
  turned off, listed by name and what is off; a click turns that one
  back on (dim with nothing silenced). Pointer hover does nothing.
- **Battery**: Pixel's battery, transcribed from SystemUI's Compose
  battery — a 24 x 13 round-rect body (13 px beside the bar's 12 px
  text) filled to the level over a translucent track, the number inside
  in BatteryGlyph's own digits, the cap one unit off — white, red at
  20 % or below, green charging (the bolt) or held at the charge limit
  (the shield), yellow in power save (the plus); an unknown level draws
  the question. The glyph overlaps the body inside a ring cut out of it,
  so the bar's glass shows through. A click does nothing. It watches the
  udev battery packs (a
  dual-pack laptop reads as one; a USB-C port's or a headset's battery
  uevent is ignored); plugging and unplugging post a capsule, and 20 %
  and 5 % post the low and critical cards.
- **Clock**: "Mon Oct 26 01:12" in the session's `LC_TIME`. A click does
  nothing: the bell is the center's door. It ticks on the wall clock's minute,
  re-reads the timezone each tick, and a resume or a clock step updates
  it at once.
- Labels are one line: a client's title or menu label has its control
  characters replaced before it is rastered (a newline per paragraph
  once made a texture tens of thousands of px tall), and a raster the
  cache refuses is remembered as empty rather than rebuilt each frame.
- Task, tray, menu and launcher icons decode under a time budget: a
  menu or launcher full of new icons fills in over a few frames rather
  than stalling one. A config reload keeps the resolved icons unless
  the GTK icon theme changed; only the misses are dropped (an icon
  installed since then still lands).

### The launcher (menubar)

awesome's Mod+P prompt, dmenu-style: a strip of the bar's own glass
right under it — a quiet "Run:" (or the drilled-into category), the
query around a drawn accent caret, then the matches 20 px apart, each a
16 px icon and its name, the selection in accent text.

The launcher lists the plugin's one desktop index (shared with the
bar's and the cards' icons), scanned once at plugin start in a helper
process (symlinked roots followed); the next open after an
applications directory changed (an app installed or removed since)
rescans it, the old list shown until the new one lands whole; a scan
cut short (its 30 s deadline, a failed helper) is retried at the next
open. A launcher open during the first scan lists what has landed and
the rest once the scan ends; a rebuild keeps the selected row (an app,
a category, or the Exec entry). Completion runs in the same kind of
helper. Desktop Entry strings are decoded
before use (space around `=` is ignored, per the spec); `Exec=`
preserves its quoting and field-code grammar before passing argument
boundaries to the executor.

The typed line runs through the executor's `/bin/sh -c`, so a
completed name is inserted as one escaped word (a file named
`x$(cmd)` stays a file name; spaces, quotes and `=` are backslashed),
and a name with a control or bidi character is never offered. An
escaped space keeps a word whole for the next completion. The command
word completes from `$PATH`, or from its own directory when it is
typed as a path (`./run`, `~/bin/x`: executables and directories);
the first Tab applies the first of the whole sorted scan (or its 512
cap, or what two seconds gathered from a stalled mount), not of the
first batch to land. Every launch
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
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace`, `Delete` | edit |
| `C-v` | paste asynchronously |
| `Escape`, or `BackSpace` on an empty query | leave an opened category |
| `Escape` (no category) or pointer click | close |

Typed and pasted input shares a 4 KiB UTF-8 limit (completions too);
a paste drops control and bidi characters. Clipboard transfer
is nonblocking with a 1.5-second deadline; closing or tearing down the
launcher removes the fd and invalidates the callback. History lives in
the plugin's one state file (see State).

### Windows

- **Minimize**: Hyprland has no minimized state of its own, so the
  windows module takes a minimized window out of layout and render and
  remembers its fullscreen, maximize and floating state for the
  restore. Focus cycling skips minimized windows.
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
  slot is the group's). Minimizing the focused window focuses the
  workspace's most recently focused task (awesome's autofocus), that
  exact window and unraised, even where another window lies over its
  center. A plugin reload keeps both states: minimized
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
Newest at the top, in the Frost material: one card shape everywhere (a
banner, a center card, a card in a stack), 356 px of regular glass at an
18 px superellipse radius, a soft drop and a light rim. The system's
feedback (volume, brightness, microphone, touchpad, the plug edges) is a
capsule instead: 256 x 36 at the top center, a glyph and a level with
its number, or a glyph and words — "Muted" in the urgent colour.

**Spec surface.** Methods `Notify`, `CloseNotification`,
`GetCapabilities`, `GetServerInformation` (spec 1.3); signals
`NotificationClosed`, `ActionInvoked`, `ActivationToken`,
`NotificationReplied`. Capabilities: `actions`, `body`,
`body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`inline-reply`, `persistence`, `sound` (no `action-icons`: as macOS's,
actions are menu rows, and a row is words).

All signals leave in the order the cards produced them (a card's close
never overtakes its action). `ActionInvoked`, `ActivationToken` and
`NotificationReplied` go to the notification's SENDER only, never
broadcast: what the user types into a reply, and a token that can take
the focus, are nobody else's on the session bus. `NotificationClosed`
stays a broadcast.

- `replaces_id` updates a card in place, keeping its stack slot; an
  unknown id creates the card under that id. In a chat card each message
  is a notification of its own (below): its id names that message, which
  a replace rewrites in place and `CloseNotification` takes alone. A
  replace always answers with its `replaces_id`: a new `message-id` it
  brings joins the chat sharing that id, so a client that keeps one id
  for its chat closes all of it. A card belongs to the bus name that
  created it (its replies, actions and token go there): another client's
  `replaces_id`, or a conversation merge from it, starts a fresh card
  instead, as Plasma does. A card whose sender left the bus is
  ownerless, and the app's next instance continues it (meanwhile its
  reply field, menu actions and primary are gone: they would reach
  nobody). The 9990s are the private OSD band: its ids, live or not, are
  the plugin's own in-process posts (the system module's OSD cards), so
  no client can pin a band id, take over the volume card on screen, or
  ride the band's DND bypass. Fresh ids outside the band are minted from
  a low counter.
- `expire_timeout`: 0 → sticky, >0 → ms; −1 (server decides) →
  `timeout_low` (4 s) for self-declared ephemerals (low urgency,
  `transient`, `value` cards) and `timeout_normal` (5 s) for the rest,
  after which the banner retreats to the shade (either set to 0:
  sticky). A critical card sticks until dismissed, whatever it asks
  for.
- Hints honored: `urgency`; `value` (0–100) draws the progress bar (the
  volume/brightness OSD); `image-data`/`image_data`/`icon_data` raw
  pixmaps; `image-path`/`image_path`; `desktop-entry`; `resident`;
  `transient`; `sound-file`/`sound-name`/`suppress-sound`;
  `x-canonical-append`; `x-notify-group-key`. A boolean hint is read
  the way clients send it: a bool, an int or a byte (`notify-send -h
  int:transient:1`), or a string.
- A hint of a type the daemon does not read is ignored, and so is
  every basic hint past the 64th and every action string past the 64th.
  A `Notify` past the daemon's walk bound (65,536 elements it would
  pass over, or a big-endian sender's image-data past 256 KiB) is refused
  with a D-Bus error rather than stalling the compositor; how the call
  is read is in the design doc (Bus).
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
- Without a conversation-id the legacy merge stands: a fresh `Notify`
  whose app + summary matches a live chat card joins it — triggered by
  the fd.o conversation categories (`im.*`/`call.*`, where the summary
  is the sender or the room) or `x-canonical-append`. This is how
  Telegram Desktop posts: a `Notify` per message, the chat's name as
  the summary, the sender as a bold line of its own leading a group's
  message (`<b>Alice</b>` then the message), the chat's picture (never
  the sender's) as `image-data`. Cards that vanish on expiry never merge.
- Every chat card, whichever contract merged it, keeps a LOG of its
  messages (32 retained), and each message is a notification of its
  own: its `Notify` gets its own id back (the card's first message the
  card's id). The card acts as its newest message, as macOS's stack
  acts as its top notification: Reply quotes it, Open and Mark as read
  act on it. A sender closing one id takes the messages that answer
  for it, the card going with its last; the card's own id, once its
  message is gone (closed, or out of the 32), closes nothing. The
  card's dismissal closes every id it holds. (Telegram keeps a record
  per message and answers a signal
  with the first record holding its id: one shared id made Reply quote
  the chat's oldest message.)
- The card's body is the TRANSCRIPT, CHRONOLOGICAL: the newest message
  ends it. Each message is one paragraph — a group's sender leading it
  in bold, its own line breaks kept inside it, every line that
  continues it hanging 12 px, so a line at the margin always begins a
  message. The banner and the center show what fits of the seven
  newest from the newest END, measured by the very layout that draws
  them: the newest whole (cut at its end only when it alone overflows),
  older ones above it while they fit, the first that does not keeping
  the head that fits (its sender and at least 12 characters) behind an
  ellipsis — never a message's tail. A message that repeats the one
  before it, sender and all (Telegram's hidden preview, "You have a new
  message", for each), shows once. A folded card shows the newest
  message on one line, its sender inline.
- A new message is news: the chat moves to the front of the banner
  column and the center, springing in when it rises (one already
  leading the column changes its words in place); an edit, a replace
  or a progress step keeps its place. `message-id` upserts (an edit
  replaces in place), `message-time` orders, `message-historic` marks a
  backfill (it does not grow the unread count). The count beside the
  title is `unread-count`, else the messages that arrived (a closed one
  leaves it); past one it shows as a pill. A `group` chat with no
  picture of its own leads with a facepile of its two newest distinct
  senders, each avatar its `sender-icon` or a deterministic initials
  face; a chat that sent its picture leads with it.
- `x-notify-group-key` (opaque, ≤512 bytes) sub-keys the app's stack
  (macOS's thread identifier): two or more cards of one app stack in
  the center per declared group, and the deck, the spread and the
  dismissal all act on that group alone. A replace without the hint
  keeps the group for the same app.

**Markup.** The body renders the whitelisted Pango subset
`<b> <i> <u> <span> <br>`, a `<span>` keeping only its colours
(foreground, background, their alphas), weight, style, underline and
strikethrough attributes (a font, face, size or lang per span is a
fontconfig match per value on the compositor thread); other tags are
dropped and a stray `<`/`&` that forms no tag or entity survives as
literal text, so a markup-aware sender and a naive one both come out
right. Malformed markup falls back to plain text. The title is the
spec's plain text: a tag there stays literal ("Mail from Ann
<ann@x.org>" keeps the address). `<a href>` in the body is a
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
(core/icons.hpp, from GTK's own `icon-theme.cache` where a theme has
one). A burst of distinct names a theme without a cache would be
walked for resolves the rest a beat later: the card keeps the name and
resolves it again, its sender faces too. A `desktop-entry` hint additionally
resolves the .desktop file's own `Icon=` (all `$XDG_DATA_DIRS/
applications` entries are indexed once at plugin start by a bounded
helper process, keyed by the desktop id, the file name and
`StartupWMClass`), which
beats an icon-name collision; while the index has not reached the
entry, the icon-name stand-in draws and the index upgrades the live
card when it lands. Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP + SVG
(no GIF; AVIF and JXL files are refused — the admission reads a
bounded raster size from the header, and those headers do not carry
one it parses); big images downscale once at load, not per frame, off
the compositor thread (the design doc's raster worker). A card holds
`image-data` pixels until its first upload, and past 32 MiB of them
model-wide (coalesced, resident, DND-held cards) the oldest keep an
icon's size instead. A file is opened once and judged on that open
descriptor (a regular file, bounded size, a bounded declared raster),
so a FIFO or a swap after the check cannot reach the decoder. File
icons decode asynchronously (a sender's SVG too, rastered at the hero
width when it may be one; a symbolic icon tinted on a copy): a card
whose icon is still decoding keeps the icon column reserved — the text
does not re-flow when it lands. A picture — 256 px in both dimensions
and a picture's shape, a fifth longer one way than the other, wide or
portrait — renders popup-width as a cover-cropped hero; a square is a
face or an icon at any size (Firefox posts a page's icon at its own
size, 512 px app icons and avatars among them). The shade keeps its rows
compact and shows the same image as a square-cropped thumbnail in the
icon column (AOSP's collapsed BigPicture). Iconless cards draw a random
face from `fallback_icon_dir` (listed recursively by a helper process,
up to 4096 images; until the listing lands they wear the generic mark),
and when that dir is empty (or a face fails to load) a deterministic
generic application mark, so no card is faceless. The icon column is Android's conversation container: the
CONTENT image leads as the avatar (a true circle for a conversation —
unless the picture came shaped, its corners clear, as Telegram's forum
pictures are rounded squares — a rounded square otherwise) and the
IDENTITY (`app_icon`/`desktop-entry`) rides its bottom-right corner as
a round badge: an 18 px disc on the
36 px face, a 1.5 px white ring that cuts it free of a light face and a
dark one alike, the app glyph on a graphite ground. Only a real
identity that differs from the face badges; a card with no content
image leads with its identity and wears no badge. A group chat without
a picture leads with a facepile of two participants. One column says
both who sent it and which app carried it.

**Actions.** As macOS 27's, a card wears no buttons at rest, and none
under the pointer either: macOS's hover close button and Options chip
are left out, the hold and the right click doing their work (the
glass alone lifts under the pointer). The card menu — held left —
lists what Options would: the sender's own rows first — Reply, then
each non-`default` action in the sender's order — over the system's
switches. A row emits `ActionInvoked` and dismisses the card unless
the `resident` hint holds it. The `default` action (and a lone action,
which keeps its row too) fires on a body left click, on BOTH surfaces,
and gets no row.
`ActivationToken` precedes each invoke (a
compositor-minted xdg-activation token, spec 1.3 ordering); the invoke
also focuses the sender's own window itself, for every backend — a
Wayland sender that spends the token lands on its already-focused
window (vanilla), a Wayland sender that never does, and any X11 sender
(whose own activation is unauthenticated and maps to urgency), still
get the window the click meant (the focus contract, README focus
semantics).

**Behavior.**

- The mouse, on every card: LEFT acts, on its release over the card it
  pressed — the link, or the card's primary
  (the `default`), then the card goes unless `resident`; a card the
  center folded opens instead, and a stack's deck spreads. HOLD left
  (450 ms) opens the card menu at the pointer. RIGHT dismisses (a
  stack's deck or header: the whole stack). MIDDLE does nothing —
  swallowed, so the window under a card never gets a paste. The cards
  own the pointer over them; hover never leaks to the window beneath.
- The card menu (macOS's notification menu, what fd.o can do): the
  sender's Reply and actions, then "Mute
  <chat> for 1 Hour" and "Mute <chat> Until Tomorrow" (08:00) for a
  conversation, "Unmute <chat>" while one holds; "Turn Off <App>
  Banners" and "Silence <App> Sounds", which read "Turn On…" and
  "Allow…" once off; "Close All from <App>". Press, hold, slide onto a
  row and let go, or click a row of the open menu; any other press,
  Esc, the lock or a native capture closes it, and it holds its card's
  banner while up. The switches persist in the state file
  (`notify-app`, `notify-mute`): an app with banners off, or a muted
  chat, lands its arrivals silently in the shade (the bell's dot marks
  them; a muted chat makes no sound either), and turning banners off sends the
  app's live banners there at once. Critical punches through.
- Popups per app (`coalesce_popups`, 3): while an app shows that many
  banners on their clocks, its next non-critical card lands in the
  shade silently (the bell's dot marks it, no sound). A banner with no clock
  (sticky, critical) is no cooldown, and two chats are two cards: each
  alerts.
- Popup hover HOLDS the timeout: the card under the pointer stops
  counting down, and leaving restarts its full clock rather than
  resuming the sliver that was left. Only one card can be held, because
  only one can be hovered.
- The center: the control center's block, then the banners' column
  under a row of its own. The block is one glass panel the column's
  width: three tiles — Do Not Disturb, Microphone, Touchpad, each lit in
  the accent while on (the microphone while live), dim and inert with no
  such device — and a row per level the system module knows: the
  display's brightness, the keyboard light, the volume (a device that
  does not answer has no row). A click on a tile flips it; a press on a
  track sets the level there and drags it; the wheel moves it 5 % a
  notch; the speaker's glyph mutes. A level past 100 % (set elsewhere)
  shows its number over a full track: the wheel never raises it, and a
  notch down lands at 100. A right click on the volume lists its outputs
  (`wpctl status`'s sinks, the default checked) and a pick makes one the
  default; on the keyboard light it offers Follow the Room (dim without
  a light sensor). A key that moves a level while the center is open moves its
  slider, and no capsule covers the panel. Under the
  block, "Notifications" at its left, bare on the desktop with a soft
  shadow (as macOS sets text on the desktop), and at its right "Clear
  All" on a glass capsule. As macOS's, it draws no backdrop: the
  desktop is never darkened.
  Cards show their clock time where banners show an age. A card
  behaves as its banner did — left on the body fires the primary, a
  link opens, the hold opens its menu. Cards open by default as the page has
  room; one the budget folded shows its title and newest line, and a
  click (or Space) opens it. An app's cards stack, as macOS's do (two
  or more, its chats among them, each chat its own card): collapsed,
  the newest card stands over a deck of up to two slivers, 8 px a
  layer; a click spreads the stack under a header of "Show Less" and a
  close button, and right on the deck (or the header's close)
  dismisses it whole. A stack holding the armed reply stays spread. A
  spread stack taller than the page shows the cards that fit and a
  "+N more" line, and dismissing a card brings the next one in. The paging
  chips (above the fold, and below it with a count) are their own
  targets: a click pages a screenful, like the wheel.
- `max_height` bounds a popup by priority: header, title, an open
  reply field, one body line and progress first, then the images, then
  their alt text; the body takes the rest. A block that doesn't fit is
  dropped whole, never painted past the card. A wrapped body link is
  hit on exactly the text it covers, line by line.
- Acting CLOSES the shade: firing a card's primary, one of its menu's
  actions or opening a body link all raise something over the panel
  the click was made in, so the panel gets out of the way. fd.o has no
  `isActivity`, and `resident` is the nearest thing it does have, so
  the shade goes exactly when the card goes. Everything that keeps you
  here keeps the shade: a dismissal, a fold, DND, "Clear all", the
  reply field, and a card with no action to fire.
- The close returns the absorbed stack: opening the shade stands the
  live banners down into parked shade rows. An EXPLICIT close — an
  outside click, Esc, the toggle — pops those cards back to banners,
  under the same per-app cap and switches the DND resume applies, on
  fresh timeouts; a close that leaves the
  notifications invisible is a lost-notification bug. An
  action-driven close (the sender is coming up over the parked stack)
  does not re-pop.
- Inline reply (KDE's protocol, which Telegram Desktop speaks): an
  action keyed `inline-reply` is the menu's Reply (labelled with the
  sender's submit text). Reply, or Tab on the center's selection, opens
  the field in the card — a banner or a center card alike (a dark well
  whose send circle turns the accent once there is text) — and sending
  emits `NotificationReplied(id, text)` and closes the card unless
  `resident`.
  `x-kde-reply-placeholder-text` and `x-kde-reply-submit-button-text`
  are honored. The field takes the whole keyboard while armed (there is
  no focus to give it); editing is append-and-backspace plus C-u / C-w,
  and a text wider than the field shows its newest end. The armed row
  stays open and on the page; folding it closes the field. An armed
  banner holds its clock, as the hovered one does, and Esc closes its
  field. A card that dies mid-reply (the sender closed it) disarms the
  field: the key that finds it gone is swallowed and the selection
  dropped, so no keystroke reaches another card.
- Shade keys, while it is open and only then: Esc closes (an open card
  menu first), ↑/↓ move a selection (an accent ring; the page follows
  it, however tall the next row), Space folds, Enter fires the primary,
  Tab arms the selected card's reply field, Delete dismisses. Paging with the wheel or the
  chips drops the selection (Enter and Delete never act off screen).
  Modified chords pass through as user binds, and so does any key with
  nothing to act on.
- Quiet automatically (the bell's menu; both off by default, each an
  option row): over a focused fullscreen window, or while a display or a
  region of one is shared, banners and sounds hold — an arrival lands in
  the shade and the bell dots, as an app with banners off does, and the
  soft banners already up retreat there when the quiet begins (a share's
  once it has held a second, so a screenshot never moves them). Critical
  punches through, as through DND; over a share it shows its face and
  title without its words (no body, messages or images). A share is the
  compositor's own test for a consumer: a frame copied within its half
  second, or one waiting for a still screen to change; a capture client
  that stopped asking (a screenshot tool, the portal after a call) is no
  share, though the compositor keeps its session. It is rechecked each
  second while one is up. A window share never shows a card, so it never
  quiets anything.
- Over fullscreen: with quiet automatically off, banners show over a
  real fullscreen window — the ecosystem default. While a card is
  up over a fullscreen window, the plugin blocks the monitor's solitary
  render and direct scanout through the compositor's
  `monitor.blockSolitary` hook, so the card composites over it; scanout
  re-engages once the last card clears. The open menubar does the same.
- Captures: a monitor capture shows the bar, menus and cards (they are
  part of its scene); a workspace capture (the image-capture source of
  one workspace's windows) never does.
- Critical: "Critical" in the urgent colour where the meta goes, an
  urgent progress fill, never expires.
- Sound: `sound-file`/`sound-name` play through a player
  (`sound_command`, empty disables); `suppress-sound` mutes one
  arrival. The compositor has no audio backend, so this shells out,
  reaped off the event loop: at most 4 players at once (each one's
  process group killed after 30 s, so a wrapper script's player goes
  with it), a budget of their own — a sound flood never takes the
  volume keys' helpers.
- DND (`hl.plugin.awesome.suspend()`, the center's tile, or the bell's
  right click): arrivals
  collect silently with timeouts held, and resume renders the queue
  newest-first on fresh timeouts, under the per-app cap and switches.
  DND is remembered (the `dnd` option): a relog or a plugin update keeps
  it. `suspend(minutes)` holds it that long from now (0 ends it); the
  end is a wall-clock time, so a laptop asleep past it wakes with DND
  off, and the end resumes the queue by itself.
  An update or a conversation message onto a card already shown (a
  banner, or a row in the shade) updates it in place, silently, as it
  was.
- Residency (`persistence`): an expired banner RETREATS into the shade
  rather than closing, and waits there until dismissed or acted on —
  the shade is the safety net. There is no history and no recall: a
  dismissed card is gone.
- A `transient` card (the spec's "bypass persistence": a screenshot
  tool's preview, a client's own OSD) is feedback, never a
  notification: it shows as a popup while it lasts, then vanishes —
  no shade row, no residency, no dot on the bell. Opening the shade ends a live
  one. A critical transient
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
pragmatic chain (GTK theme → hicolor → pixmaps), not a full
`index.theme` inheritance engine.

### System

- **The lights**: the display's brightness reads `/sys/class/backlight`
  (several devices: the firmware one, then platform, then raw — the
  systemd rule), the keyboard light the LED the kernel names
  `*kbd_backlight` (`/sys/class/leds`, the first by name). Both apply 5%
  linear steps (the display with a raw floor of 2, the keyboard down to
  off) and write through logind `Session.SetBrightness` on the system
  bus — no root, no udev rule. One write is in flight per light;
  presses meanwhile add up into the next. A device that is missing or
  stops reading is looked up again on the next press (a late driver, a
  GPU switch); with none, or no system bus, the press answers with an
  "unavailable" card. Keyboards with light keys bind
  `keyboard_light_up` / `keyboard_light_down`.
- **The keyboard light in the dark**: while every display is off (DPMS —
  an idle daemon's blank, `hl.dsp.dpms`), the keyboard light is out,
  quietly; the first display back gives it its level back exactly. A
  choice made in the dark (a key, the slider) stands: the wake leaves
  it. A locked screen keeps its light: a password is typed in the dark.
- **Follow the Room** (the keyboard light's menu; off until chosen, and
  remembered): the keyboard light is on in a dark room and off in
  daylight, as iio-sensor-proxy's ambient light sensor reads it (claimed
  only while followed). Dark is under 20 lux, light over 60, and a new
  reading must hold 5 s before the light follows, so a hand over the
  sensor never flickers it. On is the level you last chose (50 % before
  any). A choice by hand (a key, the slider) stands until the room
  actually changes; dark displays win over the room. A sensor that
  reports no lux has no scale to judge by: the option stays dim.
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
- **A headset's own volume**: a Bluetooth headset that sets its volume
  itself (its buttons, a touch strip — AVRCP absolute volume) presses no
  key, so no action runs. BlueZ announces the transport's new `Volume`
  on the system bus (only `org.bluez` is heard), and that reads the
  default sink back: the capsule shows only a level or a mute that
  moved, so a change on another sink, or the echo of the keys' own set,
  shows nothing. A burst reads back at most every 120 ms, one wpctl at a
  time with the keys. A headset without absolute volume sends volume
  keys (BlueZ's AVRCP input device), which reach the binds.
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
  model (fixed ids — pad 9991, brightness 9992, volume 9993, keyboard
  light 9994, mic 9995 —
  replace in place, low urgency, 1200 ms expiry, the touchpad's
  1500 ms) that names a glyph, so
  it shows as a capsule at the top center: the glyph, a level track and
  its number for a value, the words otherwise; a muted volume or
  microphone reads "Muted" / "Off" in the urgent colour. Such a card is
  feedback, never a notification: no shade row, no history, no dot
  on the bell;
  while the shade is open, its block shows the change instead and no
  capsule covers it; opening the shade drops a live one. A critical battery alert, posted in the same band,
  is an alert: it waits under the shade like any banner.

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
hl.plugin.awesome.suspend()                -- toggles DND; suspend(minutes) holds it that long
hl.plugin.awesome.clear_all()
hl.plugin.awesome.volume_up()              -- volume_down, mute, mic_mute
hl.plugin.awesome.brightness_up()          -- brightness_down
hl.plugin.awesome.keyboard_light_up()      -- keyboard_light_down
hl.plugin.awesome.touchpad_toggle()
```

## hyprctl

`hyprctl awesome <verb>` routes to the owning module: notify serves
`count`, `center`, `state`, `badge`, `topline`, `clear`; system serves
`pad`. `trace` (the input pipeline) prints the last 32 button, axis and
key events (a run of scroll events on one axis, taken by the same owner,
is one entry with its count) and who took each — a module, `blocked`
(session lock or input capture), `upstream` (cancelled before the
plugin) or `-` (passed to the compositor; `- (a passed press holds it)`
for a release a module would have swallowed): the first stop when a
click goes missing.

## Configuration

45 keys under `plugin:awesome:<module>:<key>` — shell 17, notify 25,
windows 3, as [`core/schema.hpp`](core/schema.hpp) declares them (kinds,
clamps; out-of-range values clamp). "Frost" marks a colour whose
default is a Frost token ([`core/tokens.hpp`](core/tokens.hpp)), so an
empty config still looks right; the user's `theme.lua` overrides them.

| `shell` | default | |
|---|---|---|
| `height` | 28 | bar height in logical px (reserve it: monitor reserved top) |
| `font_size` | 12 | text size in logical px (monitor scale applies at raster time) |
| `tray_spacing` | 20 | px between tray icons (macOS's menu-extra rhythm) |
| `font` | `Fira Code, IBM Plex Sans JP` | font family, or a Pango family list (the kanji tags take the first face that has them) |
| `terminal` | `alacritty` | terminal that runs Terminal=true launcher entries |
| `col_bg` | Frost | the bar's and the launcher's thin glass (the alpha IS the glass) |
| `col_menu_bg` | Frost | menus' thicker glass |
| `col_fg` | Frost | bar text and glyphs: the clock, the bell, symbolic tray icons, occupied tags |
| `col_focus` | Frost | the launcher's and the menus' selection (text) |
| `col_active` | Frost | the viewed tag and the focused task (text), the bell while the center is open |
| `col_dot` | Frost | the bell's dot: cards the center has not shown |
| `col_empty` | Frost | empty tags, minimized tasks, disabled rows |
| `col_urgent` | Frost | urgent tags and tasks (text) |
| `col_frame` | Frost | the glass rim |
| `col_charging` | Frost | the battery's fill charging or held at the charge limit |
| `col_low` | Frost | the battery's fill at 20 % or below |
| `col_powersave` | Frost | the battery's fill in power save |

| `notify` | default | |
|---|---|---|
| `font` | `Inter` | font family |
| `font_size` | 13 | body text size in logical px (the type roles derive from it) |
| `width` | 356 | card width in logical px: the banners' and the center's column |
| `max_height` | 320 | banner height cap in logical px |
| `max_icon` | 36 | the card's icon column in logical px |
| `margin` | 8 | inter-card gap in logical px |
| `offset_y` | 40 | the column's and the capsules' distance from the monitor top |
| `timeout_low` | 4000 | ephemeral timeout in ms (low urgency, transient, progress cards) |
| `timeout_normal` | 5000 | normal banner timeout in ms, then it retreats to the shade; 0 = sticky |
| `coalesce_popups` | 3 | live popups per app (0 = no cap); same-app extras land silent in the shade |
| `max_notifs` | 50 | model cap; overflow evicts the oldest non-critical card |
| `ignore_dbusclose` | 0 | ignore app-initiated CloseNotification (dunst's knob) |
| `rounding` | 18 | card radius in logical px |
| `rounding_power` | 3.0 | corner superellipse exponent |
| `sound_command` | `canberra-gtk-play` | libcanberra player for sound hints; empty disables |
| `fallback_icon_dir` | empty | iconless cards draw a random identity face from this directory |
| `col_bg` | Frost | glass fill (the alpha IS the glass) |
| `col_menu_bg` | Frost | the card menu's thicker glass |
| `col_fg` | Frost | body text |
| `col_title` | Frost | titles, a stack header's chips, the capsules' words |
| `col_kicker` | Frost | meta and secondary text |
| `col_frame` | Frost | the glass rim |
| `col_urgent` | Frost | critical: the meta, its progress, an alarmed capsule |
| `col_highlight` | Frost | the accent: progress, the selection, the reply field |
| `col_link` | Frost | body hyperlinks |

| `windows` | default | |
|---|---|---|
| `edge` | 16 | distance from an output edge that arms a snap zone |
| `snap_distance` | 8 | magnetic pull distance |
| `col_frame` | Frost | snap preview outline (its tint is the same colour at 10 %) |

## State

One file: `$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv` — typed
tab-separated rows, the key last:

| row | meaning |
|---|---|
| `spot <x> <y> <class>` | where the app's last window closed |
| `windowed <x> <y> <w> <h> <class>` | its last windowed box: the restore of a window born maximized |
| `launches <count> <name>` | the launcher's ranking, by .desktop Name |
| `history <query>` | the prompt history, newest last |
| `notify-app <flags> <label> <app>` | the card menu's per-app switches (1 banners off, 2 sounds off), listed by the app's name |
| `notify-mute <until> <label> <chat>` | a muted chat until a unix time, listed by its title (a mute that ended is dropped) |
| `option <value> <name>` | a choice the user made from a menu, written only off its default ([`core/options.hpp`](core/options.hpp)): `dnd` (-1 until turned off, else its end), `quiet-fullscreen`, `quiet-sharing`, `kbd-follows-room` (1 on) |

Positions are relative to the origin of the monitor the window was on: a
spot remembered on one monitor lands at the same place on whichever
monitor the app opens on next. A label is display text (128 bytes, its
tabs and line breaks blanked). An option row the schema does not name,
or out of its range, is dropped; the config files stay the user's, and
no option is both a config key and a row. Each kind is kept oldest-first
and bounded (1024 keys, 50 queries): at the bound the least recently
remembered row goes, never a new one. The windows still open when the plugin stops (a
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
make -C awesome test       # headless core harness (fork headers, no compositor)
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
