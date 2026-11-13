# Notifications

The `notify` module is the session's `org.freedesktop.Notifications`
daemon: banners, the center, Do Not Disturb, conversations, inline reply
and sound.

## Cards

Cards stack top-right on the focused monitor, newest on top, `offset_y`
below the top and at least 12 px under the bar. While the launcher is
open there, cards and center sit its height lower. The stack follows a
keyboard or workspace focus change, not a pointer one.

One card shape serves a banner, a center card and a card in a stack:
356 px of glass, an 18 px superellipse radius. An open tray menu paints
over the cards, as it takes the press first.

System feedback (volume, brightness, microphone, touchpad, power) is a
**capsule** instead: 36 px tall, top center, a glyph with a level track
and number (256 px) or with words up to 256 px
([system](system.md#feedback)).

### The icon column

- A content image leads as a 36 px face: a circle for a conversation
  (unless the picture came shaped), a rounded square otherwise.
- The app's identity (`app_icon`, `desktop-entry`) rides its
  bottom-right as an 18 px badge: a colour icon in a white ring, a
  symbolic one in graphite on white.
- Without a content image the identity leads, unbadged; a group chat
  without a picture leads with its initials on a colour its chat key
  picks, badged.
- An iconless card draws a random face from `fallback_icon_dir`, else a
  generic application mark.

A picture (≥ 256 px each way, at least half the card's content width in
device pixels, a fifth longer one way) is a card-wide, cover-cropped
hero; in the center it is a square thumbnail. A square image is a face
at any size.

## The mouse

| Input | Action |
|---|---|
| left (released over the same card) | the link under it, or the `default` action, then the card goes unless `resident`; with no action, dismiss. A folded card opens; a deck spreads |
| left held 450 ms | the card menu, at the pointer |
| right | dismiss (on a deck or stack header: the whole stack) |
| middle | nothing; swallowed so the window beneath never pastes |
| side buttons (back, forward) | nothing; swallowed so the window beneath never navigates |

Cards own the pointer over them: hover never leaks beneath. Hover lifts
the glass and holds the banner's clock (leaving restarts it in full).

### The card menu

The sender's rows first: Reply, then each non-`default` action in order
(eight at most). Then the switches:

- Mute *chat* for 1 Hour · Mute *chat* Until Tomorrow (the next 08:00;
  "Until 08:00" after midnight), for a conversation; Unmute *chat* while
  one holds.
- Turn Off *App* Banners · Silence *App* Sounds (Turn On… / Allow… once
  off).
- Close All from *App*.

Press, hold, slide onto a row and release; or click a row of the open
menu. Any other press, Esc, the lock or a native input capture closes
it; it holds its banner while open. An action row emits `ActionInvoked`
and dismisses the card unless `resident`.

An app with banners off, or a muted chat, lands arrivals silently in the
center (the bell dots; a muted chat makes no sound); turning banners off
moves the app's live banners there at once. The switches persist
(`notify-app`, `notify-mute` in the [state file](reference.md#state))
and are listed under the bell's Silenced Apps. Critical cards punch
through.

## Banners

- **Timeouts**: `expire_timeout` 0 is sticky, > 0 is ms, −1 means
  `timeout_low` for low urgency, transient and progress cards,
  `timeout_normal` otherwise.
- **Residency**: an expired banner retreats into the center until
  dismissed or acted on. No history: a dismissed card is gone.
- **Per-app cap** (`coalesce_popups`): while an app shows that many timed
  banners, its next non-critical card lands silently in the center.
  Sticky banners hold no slot; two chats are two cards.
- **Height** (`max_height`): header, title, an open reply field, one
  body line and progress come first, then the hero (cut to fit, dropped
  under 48 px), then images, then their alt text; the body takes the
  rest. An image or alt line that does not fit is dropped whole.
- **Transient** cards (a screenshot preview, an app's OSD) are
  feedback: a banner while it lasts, then gone, with no center row or
  dot; opening the center ends one. A critical transient never expires
  and is listed.
- **Critical** cards say "Critical" in the urgent colour, fill their
  progress in it and never expire.
- **Overflow**: past `max_notifs` the oldest short-lived card
  (transient, progress, capsule) goes, else the oldest non-critical,
  with `NotificationClosed`.
- **Fullscreen**: with quiet automatically off, banners show over a
  fullscreen window; the plugin blocks that monitor's direct scanout
  while a card (or the launcher) is up.
- **Captures**: a monitor capture shows bar, menus and cards; a
  workspace capture never does.
- **Session lock**: cards never render above the lock screen and
  repaint at unlock.

## The center

The bell's left click (or `center()`) opens it on that monitor. It
absorbs the live banners, and what arrives while open, into parked rows.
An explicit close (outside click, Esc, the bell) pops them back as
banners on fresh timeouts, under the cap and switches; a close by a
card's action leaves them parked. No backdrop.

"Notifications" heads the column, ending in Do Not Disturb's moon (a
24 px glass disc, accent glyph under the pointer, white disc while DND
holds) with "Clear All" on a glass capsule 8 px left of it (gone when
nothing to clear). Cards show clock time instead of age; a card the
center had not shown wears its time in the accent for the visit.

- **Order**: critical, conversations, the rest, newest first in each;
  16 px between groups, 8 between cards; no labels.
- **Silent cards** fold into one bottom row: low urgency, banners off, a
  muted chat (read live). It shows two app icons and "3 silent from Chat
  and Games"; a click (or Enter) spreads them for the visit, and a reply
  in progress keeps them spread.
- **Status line** under the heading: DND ("until 22:30" or "until turned
  off", and how many wait); quiet ("while an app is fullscreen", "while
  you share the screen").
- **Clear All** hides every card and reads **Undo** for 5 s. The window's
  end, or closing the center, closes them for good (`NotificationClosed`,
  dismissed); arrivals meanwhile show as ever. `clear_all()` and
  `hyprctl awesome clear` sweep at once, without undo.
- Cards open as the page has room; a folded one shows its title and
  newest line, and a click (or Space) opens it.
- An app's two or more cards stack: the newest over a deck of up to two
  slivers. A click spreads the stack under a header (the app's name,
  Show Less); right on the deck or header dismisses it whole. A stack
  holding the armed reply stays spread. `x-notify-group-key` splits a
  stack by thread.
- A spread stack taller than the page shows what fits and "+N more";
  paging chips page a screenful, the wheel one item a notch.

## Do Not Disturb

A click on the moon lists For 30 Minutes · For 1 Hour · For 3 Hours ·
Until Tomorrow (the next 08:00; "Until 08:00" after midnight) · Until
Turned Off; while DND holds, a click ends it. Binds use
`suspend([minutes])`. Arrivals collect silently with their timeouts
held; turning it off shows the queue newest first on fresh timeouts,
under the cap and switches. An update to a shown card changes it in
place, silently.

DND persists across a relog or plugin update. A timed DND ends at a
wall-clock time: a laptop asleep past it wakes with DND off.

## Quiet automatically

Two bell-menu options, off by default:

- **When an App Is Fullscreen**: the focused window is fullscreen.
- **While Sharing the Screen**: a display, or a region of one, is shared.

While either holds, arrivals land in the center (the bell dots) with no
sound, and up banners retreat there (for a share, once it has held a
second, so a screenshot moves nothing). Critical cards punch through;
over a share they show face and title without their words. Quiet cards
rank as usual.

A share counts only while a client consumes frames (one copied in the
last 0.5 s, or one waiting for a still screen to change), so an idle
portal session or a finished screenshot doesn't. A window share never
shows a card, so it quiets nothing.

## Conversations

A chat is one card however many `Notify` calls it takes.

- **Merging**: every message with the same `conversation-id` (opaque,
  ≤ 512 bytes) joins the chat's card. Without one, a `Notify` whose app
  and summary match a live chat card joins it for the `im.*` and
  `call.*` categories or `x-canonical-append`. That is Telegram
  Desktop's shape: one `Notify` per message, the chat name as summary, a
  group's sender as a leading bold line, the chat picture as
  `image-data`.
- **One id per message**: each `Notify` gets its own id, and the card
  acts as its newest message (Reply quotes it; Open and Mark as Read act
  on it), since Telegram acts on the first record holding a signal's id.
  Closing an id removes that message, the card going with its last;
  dismissing the card closes every id.
- **Transcript**: oldest to newest, a paragraph per message (a group's
  sender bold, continuation lines hanging 12 px). It shows what fits of
  the seven newest from the newest end; the first that doesn't fit keeps
  its head behind an ellipsis. A repeat of the previous message shows
  once. A folded card shows the newest on one line.
- **Order**: a new message moves the chat to the front; an edit,
  replace or progress step keeps its place. `message-id` upserts,
  `message-time` orders, `message-historic` marks a backfill that isn't
  unread.
- **Count**: `unread-count`, else the arrivals, as a pill past one.

## Inline reply

KDE's protocol, which Telegram Desktop speaks. An `inline-reply` action
becomes the menu's Reply, labelled with the sender's submit text. Reply,
or Tab in the center, opens a field in the card; its send circle turns
the accent once there is text. Sending emits `NotificationReplied(id,
text)` and closes the card unless `resident`.

The armed field takes the whole keyboard: typing, Backspace, `C-u`,
`C-w`; Esc closes it. An armed banner holds its clock. A card that dies
mid-reply disarms the field and swallows the next key.
`x-kde-reply-placeholder-text` and `x-kde-reply-submit-button-text` are
honoured.

## Sound

`sound-file` and `sound-name` play through `sound_command` (empty
disables); `suppress-sound` mutes one arrival. At most four players run,
each killed with its process group after 30 s, on their own budget so a
flood never delays the volume keys.

## Protocol

Spec 1.3. Methods `Notify`, `CloseNotification`, `GetCapabilities`,
`GetServerInformation`; signals `NotificationClosed`, `ActionInvoked`,
`ActivationToken`, `NotificationReplied`. Capabilities `actions`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`inline-reply`, `persistence`, `sound` (no `action-icons`: actions are
words).

- **Signals** leave in the order cards produced them. Actions, tokens
  and replies go to the sender only; `NotificationClosed` is broadcast.
  `ActivationToken` precedes each invoke and reply, and an invoke on a
  non-`resident` card also focuses the sender's window.
- **Ownership**: a card belongs to the bus name that created it; another
  client's `replaces_id` or merge starts a fresh card. A card whose
  sender left the bus is ownerless: the next `replaces_id` naming it, or
  the same app's next merge, takes it over.
- **Ids**: `replaces_id` updates in place, keeping the slot; an unknown
  id creates the card under that id. Ids 9990–9999 are the plugin's own.
- **Hints**: `urgency`; `value` (0–100, progress); `image-data` /
  `image_data` / `icon_data`; `image-path` / `image_path`;
  `desktop-entry`; `resident`; `transient`; `sound-file`, `sound-name`,
  `suppress-sound`; `x-canonical-append`; `x-notify-group-key` (≤ 512
  bytes). Booleans may come as bool, int, byte or string. Unknown types
  are ignored, as are hints and action strings past the 64th.
- **Conversation hints** (the plain name first, then the `x-notify-`
  alias): `conversation-id`, `conversation-title`, `conversation-kind`
  (`one-to-one` or `group`), `sender-id`, `sender-name`, `message-id`,
  `message-time` (int64 ms; alias `x-notify-message-timestamp`),
  `message-historic` (bool, or `true`/`false`, `yes`/`no`, `1`/`0`),
  `unread-count` (uint, capped at 999).
- **Bounds**: a `Notify` too large to read within the daemon's bounds
  is refused with a D-Bus error instead of stalling the compositor
  ([design](design.md#the-notification-bus)).

### Markup

The body renders Pango's `<b> <i> <u> <span> <br>`; a `<span>` keeps
only colours, weight, style, underline and strikethrough. Other tags are
dropped, a stray `<` or `&` stays literal, malformed markup falls back to
plain text. The title is one line: it keeps `<b> <i> <u> <span>`, `<br>`
becomes a space, and other tags stay literal ("Mail from Ann
<ann@x.org>" keeps the address).

`<a href>` links `http(s)://` and `mailto:` only; a click opens it with
`xdg-open` and leaves the card up. `<img src>` renders as a thumbnail row
under a banner's text, or its `alt` text if it fails to load; a center
card shows neither.

### Images and icons

`image-data` beats `image-path` beats `app_icon` beats `desktop-entry`.
The last three may be a path, a `file://` URI or an icon name
([design](design.md#icons-and-the-desktop-index)); `desktop-entry` also
resolves the entry's `Icon=` (by desktop id, file name or
`StartupWMClass`).

Images decode off the compositor thread with hyprgraphics (PNG, JPEG,
WebP, BMP, SVG) and are scaled down once at load. A card whose icon is
still decoding keeps its column, so text never reflows. No GIF (no
codec), AVIF or JXL, and no animated icons (`icon-multi`). Icon lookup
is a pragmatic chain, not a full `index.theme` inheritance engine.
