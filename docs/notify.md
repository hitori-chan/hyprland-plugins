# Notifications

The `notify` module is the session's `org.freedesktop.Notifications`
daemon: banners, the center, the control center, chat conversations,
inline reply and sounds. Disable any other daemon
(dunst, mako) before enabling the plugin.

## Cards

Cards appear top-right on the focused monitor, newest at the top,
`offset_y` below the monitor's top and never above the work area. The
stack follows a keyboard or workspace focus change, but not one the
pointer makes, so grazing the next monitor's corner leaves the cards
where they are.

One card shape serves every surface (a banner, a center card, a card in
a stack): 356 px of regular glass with an 18 px superellipse radius, a
soft drop shadow and a light rim. An open tray menu or the launcher
paints over the cards, as it takes the press first.

The system's own feedback (volume, brightness, microphone, touchpad,
power plugged) is a **capsule** instead: 256 x 36 at the top center, a
glyph and a level with its number, or a glyph and words ("Muted" in the
urgent colour). See [system](system.md#feedback).

### The icon column

The column says both who sent a card and which app carried it, as
Android's conversation layout does:

- the content image leads as a 36 px face: a circle for a conversation
  (unless the picture came shaped, as Telegram's rounded-square forum
  pictures do), a rounded square otherwise;
- the app's identity (`app_icon`, `desktop-entry`) rides its
  bottom-right corner as an 18 px badge on a graphite disc, ringed in
  white;
- a card with no content image leads with its identity and wears no
  badge; a group chat without a picture leads with its initials (the
  first letters of its title's first and last words) on a colour its
  chat key picks, wearing the app's badge;
- an iconless card draws a random face from `fallback_icon_dir`, or a
  generic application mark, so no card is faceless.

A picture (at least 256 px each way, a fifth longer one way than the
other) shows as a hero, card-wide and cover-cropped; in the center it is
a square thumbnail in the icon column. A square image is a face or an
icon at any size.

## The mouse

On every card:

| Input | Action |
|---|---|
| left (on release over the same card) | the link under it, or the card's primary (`default`) action; then the card goes unless `resident`. A folded card opens; a stack's deck spreads |
| left held 450 ms | the card menu, at the pointer |
| right | dismiss (on a stack's deck or header: the whole stack) |
| middle | nothing; swallowed, so the window beneath never gets a paste |

The cards own the pointer over them: hover never leaks to the window
beneath. Hovering lifts the glass and holds the banner's clock (leaving
restarts it in full). Cards wear no buttons, at rest or under the
pointer; the hold and the right click do that work.

### The card menu

The sender's rows first: Reply, then each non-`default` action in the
sender's order. Then the system's switches:

- Mute *chat* for 1 Hour · Mute *chat* Until Tomorrow (the next 08:00;
  "Until 08:00" after midnight) — for a conversation; Unmute *chat*
  while one holds.
- Turn Off *App* Banners · Silence *App* Sounds (Turn On… / Allow…
  once off).
- Close All from *App*.

Press, hold, slide onto a row and let go; or click a row of the open
menu. Any other press, Esc, the session lock or a native input capture
closes it; it holds its card's banner while open. An action row emits
`ActionInvoked` and dismisses the card unless it is `resident`.

An app with banners off, or a muted chat, lands its arrivals silently in
the center (the bell's dot marks them; a muted chat makes no sound);
turning banners off sends the app's live banners there at once. The
switches are remembered (`notify-app`, `notify-mute` in the
[state file](reference.md#state)) and listed under the bell's Silenced
Apps. Critical cards punch through.

## Banners

- **Timeouts**: `expire_timeout` 0 is sticky, a positive value is
  milliseconds, −1 lets the daemon decide: `timeout_low` (4 s) for low
  urgency, `transient` and progress cards, `timeout_normal` (5 s) for the
  rest. A critical card sticks until dismissed.
- **Residency**: an expired banner retreats into the center and waits
  there until dismissed or acted on. There is no history: a dismissed
  card is gone.
- **Per-app cap** (`coalesce_popups`, 3): while an app shows that many
  banners on their clocks, its next non-critical card lands silently in
  the center. A sticky banner holds no slot, and two chats are two
  cards.
- **Height** (`max_height`): a banner keeps its header, title, an open
  reply field, one body line and its progress first, then images, then
  their alt text; the body takes the rest. A block that does not fit is
  dropped whole.
- **Transient** cards (a screenshot's preview, an app's own OSD) are
  feedback: a banner while it lasts, then gone, with no center row and
  no dot. Opening the center ends one. A critical transient card is an
  ordinary alert.
- **Critical** cards say "Critical" in the urgent colour, fill their
  progress in it and never expire.
- **Overflow**: `max_notifs` caps the model. Past it the oldest
  short-lived card (transient, progress, capsule) goes, else the oldest
  non-critical one, with `NotificationClosed`.
- **Fullscreen**: with quiet automatically off, banners show over a
  fullscreen window, as elsewhere. While a card is up there the plugin
  blocks the monitor's direct scanout so the card composites; scanout
  resumes once the last card clears. The open launcher does the same.
- **Captures**: a monitor capture shows the bar, menus and cards; a
  workspace capture never does.
- **Session lock**: cards never render above the lock screen; what
  survives the lock repaints at unlock.

## The center

The bell's left click (or `hl.plugin.awesome.center()`) opens the center
on that bar's monitor. Opening it absorbs the live banners into parked
rows, and so does what arrives while it shows; an explicit close (an
outside click, Esc, the bell) pops them back as banners on fresh
timeouts, under the same per-app cap and switches, while a card's action
closing it (the app comes up) leaves them parked.
It draws no backdrop: the desktop is never darkened.

"Notifications" heads the column, bare on the desktop with a soft
shadow, and "Clear All" sits on a glass capsule (gone with nothing to
clear; its word turns the accent under the pointer). Cards show their
clock time where banners show an age, and behave as their banner did; a
card the center had not shown when it opened wears its time in the
accent for the visit.

- **The order**: critical, then the conversations, then the rest, newest
  first in each, 16 px apart where a group ends (8 between cards); no
  labels.
- **Silent cards** fold into one row at the bottom: low urgency, an app
  whose banners the card menu turned off, a muted chat (read live: a
  switch turned back on lifts its cards out). The row shows two of their
  apps' icons and "3 silent from Chat and Games"; a click (or Enter)
  spreads them for the visit, and a reply being typed into one keeps
  them spread. Quiet automatically holds banners a while and is no
  silence.
- **The status line**: while Do Not Disturb holds, a line under the
  heading says so ("until 22:30" or "until turned off", and how many
  arrivals wait); while quiet automatically holds, "Quiet" and why
  ("while an app is fullscreen", "while you share the screen").
- **Clear All** hides every card at once and reads **Undo** for 5 s;
  Undo brings them back. The window's end closes them for good
  (`NotificationClosed`, dismissed), and so does closing the center. An
  arrival meanwhile shows as ever. The scripted `clear_all()` and
  `hyprctl awesome clear` sweep at once, with no undo.
- Cards open as the page has room; a folded one shows its title and
  newest line, and a click (or Space) opens it.
- An app's cards stack (two or more, each chat its own card): the newest
  stands over a deck of up to two slivers. A click spreads the stack
  under a header with the app's name and Show Less; right on the deck or
  the header dismisses it whole. A stack holding the armed reply stays
  spread.
- `x-notify-group-key` splits an app's stack by thread, as macOS's
  thread identifier does.
- A spread stack taller than the page shows what fits and "+N more";
  the paging chips above and below the fold page a screenful, like the
  wheel.

## The control center

The bar's switches (right of the bell), `hl.plugin.awesome.control_center()`
or `hyprctl awesome controlcenter` open it on that bar's monitor, in the
cards' column; it and the center share the column, one open at a time.
Banners keep coming, 12 px under it. An outside click, Esc or its
switches close it. Every control shows only with its hardware, checked
again whenever the panel opens and as devices come and go.

**The levels**: a glass module per level a device answers for, 12 px
apart: the **Display** (a backlight), the **Keyboard** light (a
`*kbd_backlight` LED) and the **Sound**. A module reads its name and
value ("45%", "Muted") over a slider: a glyph, then the track to the
module's right edge with a white knob. A light of ten levels or fewer (a
ThinkPad's keyboard: off, low, high) snaps its knob to them and still
reads a percent.

- the keyboard's glyph (a half sun on a line) puts its light out, and
  back on at the level it last had;
- the speaker mutes and unmutes; its waves follow the level, crossed
  while silent;
- **Follow the Room**, a disc in the keyboard's title row (lit while on;
  shown with a light sensor, see [system](system.md#follow-the-room));
- the **output** pill in the sound's title row (the current output's
  kind and a chevron; shown with two outputs or more) opens **Output**:
  `wpctl status`'s sinks, the default checked, each with its kind; a
  pick makes one the default.

| On a level | Action |
|---|---|
| press on the track | set the level there (a stepped light: the nearest level), and drag it |
| press on the name or the value | nothing |
| wheel | step it as its keys do |

A level past 100 % (set elsewhere) shows its number over a full track. A
key that moves a level while the control center is open moves its
slider, and no capsule covers the panel; the microphone's and the
touchpad's keys keep their capsules (the panel has neither).

**The capsules**, under the modules: **Bluetooth** left (with an adapter;
see [system](system.md#bluetooth)) and **Do Not Disturb** right, alone
across without an adapter. A capsule's circle switches it (white with
the accent deepened for its glyph while on); its words — the name over
its state and a chevron — open its menu, the state brightening under the
pointer (the glass never lightens):

- Bluetooth's state is Off, On, the one connected device's name, "2
  Devices", "Connecting…" or "Disconnecting…"; a refusal in red ("Didn't
  Answer", "Pairing Lost", "Didn't Connect", "Didn't Turn On") until the
  panel closes; "Blocked", in red, while a hardware switch holds the
  radio off (its circle leaves it, and the menu's header says "· Off by
  its hardware switch"). A soft block (`rfkill block`, airplane mode) the
  circle lifts, then powers the radio. Its **Devices** menu lists the
  paired ones ("· Bluetooth is off" in the header while off): the
  device's kind in a disc, lit while connected, its name, and on the
  right "Connecting…",
  "Disconnecting…" or, while connected, its battery ("80%" beside a
  battery glyph, red at 20 % or less). A refused connect's row says why
  under the name ("Didn't answer." in red, then "Is it on and nearby?").
  A pick disconnects a connected device, else connects it (powering the
  radio first while off). A right-click on a device (or Delete with the
  pointer on it) asks, in place of its row, "Forget it? Using it again
  takes pairing it again." — **Forget** (red) unpairs it, **Cancel**
  keeps it; the menu stays open. A row whose pairing was lost pairs it
  again: its click forgets the stale keys, scans, and pairs the device
  once the scan finds it.
  Under the paired ones, while the radio is on, **Other Devices**: a
  **Scan** pill (Stop while it runs, eight turning dots beside it) and
  what the scan found, nearest first, at most 6; a note under them says
  what to do ("Put a device in pairing mode, then scan.", "Looking for
  devices in pairing mode…", "Nothing found. Is it in pairing mode?").
  A found device's pick pairs it, the menu staying open: "Pairing…", or
  in place of its row the code to type on it ("Type this code on it, then
  press Return", the digits already typed dimmed) or to compare ("Does it
  show the same code?" — **Pair** or **Cancel**). A paired device is
  trusted, then connected; a refused pairing reads "Pairing failed. Is it
  in pairing mode?". The menu closing stops the scan, forgets what it
  found and cancels a pairing that was asking; one that asks nothing
  runs on.
- Do Not Disturb's state is Off, On or "Until 21:30". Its menu: For 30
  Minutes · For 1 Hour · For 3 Hours · Until Tomorrow (08:00) (after
  midnight, "Until 08:00": the next 08:00) · Until Turned Off.

**Names**: the small buttons (the keyboard's glyph, the speaker, Follow
the Room, the output pill) name themselves after the pointer rests 500 ms
on them — "Light Off", "Unmute", "Follow the Room", "Output · Speakers"
— in a dark note to their left, outside the panel for a slider's glyph.
A click hides the name until the pointer comes back.

## Do Not Disturb

From the control center's capsule (its circle, or a duration from its
words) or `hl.plugin.awesome.suspend([minutes])`. Arrivals collect
silently with their timeouts held; turning it off shows the queue
newest first, on fresh timeouts, under the per-app cap and switches. An
update to a card already shown changes it in place, silently.

DND is remembered across a relog or a plugin update. A timed DND ends at
a wall-clock time: a laptop asleep past it wakes with DND off and the
queue shown.

## Quiet automatically

Two options on the bell's menu, both off until checked:

- **When an App Is Fullscreen**: the focused window is fullscreen.
- **While Sharing the Screen**: a display, or a region of one, is being
  shared.

While either holds, banners and sounds hold: an arrival lands in the
center and the bell dots, and the banners already up retreat there (a
share's after it has held a second, so a screenshot never moves them).
Critical cards punch through; over a share they show their face and
title without their words.

A share counts only while a client is consuming frames (one copied in
the last half second, or one waiting for a still screen to change): a
screenshot tool, or a portal after a call, no longer counts though its
session stays open. A window share never shows a card, so it quiets
nothing.

## Conversations

A chat is one card however many `Notify` calls it takes.

- **Merging**: a `conversation-id` hint is the contract: every message
  with that id joins the chat's card (opaque, refused above 512 bytes).
  Without one, a new `Notify` whose app and summary match a live chat
  card joins it, for the fd.o `im.*` and `call.*` categories or
  `x-canonical-append`. That is how Telegram Desktop posts: one `Notify`
  per message, the chat's name as the summary, a group's sender as a
  leading bold line, the chat's picture as `image-data`.
- **One id per message**: each message's `Notify` gets its own id back,
  and the card acts as its newest message (Reply quotes it; Open and Mark
  as Read act on it). Closing one id takes that message, the card going
  with its last; dismissing the card closes every id it holds.
- **The transcript**: the body reads oldest to newest. Each message is a
  paragraph (a group's sender in bold, continuation lines hanging
  12 px). The card shows what fits of the seven newest from the newest
  end: the newest whole, older ones above while they fit, and the first
  that does not fit keeps its head behind an ellipsis. A message that
  repeats the one before it shows once. A folded card shows the newest
  message on one line.
- **Order**: a new message moves the chat to the front, springing in
  when it rises; an edit, a replace or a progress step keeps its place.
  `message-id` upserts, `message-time` orders, `message-historic` marks
  a backfill that does not count as unread.
- **Count**: `unread-count`, else the messages that arrived, shown as a
  pill past one.

## Inline reply

KDE's protocol, which Telegram Desktop speaks. An action keyed
`inline-reply` becomes the menu's Reply, labelled with the sender's
submit text. Reply, or Tab in the center, opens a field in the card: a
dark well whose send circle turns the accent once there is text. Sending
emits `NotificationReplied(id, text)` and closes the card unless
`resident`.

The field takes the whole keyboard while armed: typing, Backspace,
`C-u`, `C-w`; Esc closes it. An armed banner holds its clock. A card
that dies mid-reply disarms the field and swallows the next key.
`x-kde-reply-placeholder-text` and `x-kde-reply-submit-button-text` are
honoured.

## Sound

`sound-file` and `sound-name` play through `sound_command`
(`canberra-gtk-play`; empty disables); `suppress-sound` mutes one
arrival. At most four players run at once, each killed with its process
group after 30 s, on a budget of their own, so a flood never delays the
volume keys.

## Protocol

Spec 1.3: methods `Notify`, `CloseNotification`, `GetCapabilities`,
`GetServerInformation`; signals `NotificationClosed`, `ActionInvoked`,
`ActivationToken`, `NotificationReplied`.

Capabilities: `actions`, `body`, `body-markup`, `body-hyperlinks`,
`body-images`, `icon-static`, `inline-reply`, `persistence`, `sound`.
No `action-icons`: actions are menu rows, and a row is words.

- **Signals** leave in the order the cards produced them. Actions, the
  activation token and replies go to the notification's sender only;
  `NotificationClosed` is a broadcast. `ActivationToken` precedes each
  invoke, and the invoke also focuses the sender's window itself.
- **Ownership**: a card belongs to the bus name that created it. Another
  client's `replaces_id`, or a merge from it, starts a fresh card. A card
  whose sender left the bus is ownerless until the app's next instance
  continues it.
- **Ids**: `replaces_id` updates a card in place, keeping its slot; an
  unknown id creates the card under that id. Ids 9990–9999 are the
  plugin's own capsules, unavailable to clients.
- **Hints**: `urgency`; `value` (0–100, a progress bar);
  `image-data` / `image_data` / `icon_data`; `image-path` / `image_path`;
  `desktop-entry`; `resident`; `transient`; `sound-file`, `sound-name`,
  `suppress-sound`; `x-canonical-append`; `x-notify-group-key` (≤512
  bytes). A boolean may come as a bool, an int, a byte or a string.
  Unknown hint types are ignored, as are hints and actions past the
  64th.
- **Conversation hints** (outside the spec's table; the plain name is
  tried first, then the `x-notify-` alias): `conversation-id`,
  `conversation-title`, `conversation-kind`
  (`one-to-one` / `group` / `call`), `sender-id`, `sender-name`,
  `sender-icon`, `message-id`, `message-time` (int64 ms; its alias is
  `x-notify-message-timestamp`), `message-historic` (bool),
  `unread-count` (uint, capped at 999).
- **Bounds**: a `Notify` the daemon would need more than 65,536 elements
  to read, or a big-endian image past 256 KiB, is refused with a D-Bus
  error instead of stalling the compositor.

### Markup

The body renders Pango's `<b> <i> <u> <span> <br>`; a `<span>` keeps
only colours, weight, style, underline and strikethrough. Other tags are
dropped, a stray `<` or `&` stays literal, and malformed markup falls
back to plain text. The title is plain text ("Mail from Ann <ann@x.org>"
keeps the address).

`<a href>` is a link for `http(s)://` and `mailto:` only; a click opens
it with `xdg-open` and leaves the card up. `<img src>` renders as a
thumbnail row under the text, or its `alt` text if it fails to load.

### Images and icons

`image-data` beats `image-path` beats `app_icon` beats `desktop-entry`.
Each of the last three may be a path, a `file://` URI, or an icon name
resolved through the GTK icon theme, then hicolor, then
`/usr/share/pixmaps`. `desktop-entry` also resolves the entry's own
`Icon=` (by desktop id, file name or `StartupWMClass`).

Images decode off the compositor thread with hyprgraphics: PNG, JPEG,
WebP, BMP and SVG. Large images are scaled down once, at load. A card
whose icon is still decoding keeps its column reserved, so the text does
not reflow.

## Limitations

- GIF does not decode (hyprgraphics has no codec); AVIF and JXL are
  refused.
- No animated icons (`icon-multi`).
- Icon lookup is a pragmatic chain, not a full `index.theme` inheritance
  engine.
