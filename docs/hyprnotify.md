# hyprnotify details

The compositor owns `org.freedesktop.Notifications`: no external daemon, no
layer surface. Cards render top-right on the focused monitor, newest at the
top, styled like the old naughty boxes (flat dark cards, 1px frame, big
icons).

## Spec surface

- Methods: `Notify`, `CloseNotification`, `GetCapabilities`,
  `GetServerInformation` (spec 1.3). Signals: `NotificationClosed`,
  `ActionInvoked`, `ActivationToken`, `NotificationReplied`.
- Capabilities: `actions`, `action-icons`, `body`, `body-markup`,
  `body-hyperlinks`, `body-images`, `icon-static`, `inline-reply`,
  `persistence`, `sound`.
- `replaces_id` updates a card in place, keeping its stack slot; an unknown
  id creates the card under that id (the OSD scripts pin fixed ids in the
  9990s, which fresh ids never mint into).
- `expire_timeout`: 0 → sticky, >0 → ms. −1 (server decides) → normal and
  critical cards are sticky until dismissed — a message waits to be read;
  self-declared ephemerals (low urgency, `transient`, `value` cards) run
  `timeout_low`. `timeout_normal` > 0 restores a clock for the rest.
- Hints honored: `urgency`; `value` (0–100) draws the progress bar (the
  volume/brightness OSD); `image-data`/`image_data`/`icon_data` raw pixmaps;
  `image-path`/`image_path`; `desktop-entry`; `action-icons`; `resident`;
  `transient`; `sound-file`/`sound-name`/`suppress-sound`.

## Markup

- Body and title render the whitelisted Pango subset: `<b> <i> <u> <span>
  <br>`. Other tags are dropped; a stray `<`/`&` that forms no tag or entity
  survives as literal text, so a markup-aware sender and a naive one both
  come out right. Malformed markup falls back to plain text.
- `<a href>` in the body is a hyperlink (Pango has no `<a>` tag, so it is
  rewritten to a styled span and hit-tested by its stripped-text byte
  offset); a click opens the URL via `xdg-open` and leaves the card up — but
  not the shade, since a browser is about to cover it. The pointer shows the
  hand over a link.
- `<img src>` in the body renders as a thumbnail row below the text.

## Images / icons

- Precedence: `image-data` beats `image-path` beats `app_icon` beats
  `desktop-entry`.
- Each of `app_icon` / `image-path` / `desktop-entry` may be a file path
  (`file://` too) OR a freedesktop icon NAME, resolved against the GTK icon
  theme, then hicolor, then `/usr/share/pixmaps`.
- Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG (no GIF); big
  images downscale once at load, not per frame. Raw `image-data` is downscaled
  directly into its bounded output buffer, without a full-size decoded copy.
  Wide images (aspect ≥ 1.5) render card-width as a cover-cropped hero.
  Iconless cards draw a random face from `fallback_icon_dir`.
- The icon column is Android's conversation container
  (`notification_2025_conversation_icon_container.xml`): the CONTENT image
  leads as the avatar — a true circle for a conversation, a squircle
  otherwise — and the IDENTITY (`app_icon`/`desktop-entry`) rides its
  bottom-right corner as a badge, sized by AOSP's ratios off the 40dp
  avatar: a 20dp badge of which **16dp is the app glyph and 2dp on each
  side is the rim**, placed so the glyph sits flush with the avatar's
  bottom-right corner (AOSP spells that margin out as 40 − 16 − 2 = 22dp)
  and only the rim protrudes. The 2021 template we took these from spent
  4dp on the rim and left 12dp for the glyph; at a 40px icon that is a 10px
  app icon inside a 3px ring, and AOSP itself halved the rim and grew the
  glyph. The disc is a near-white `BADGE_RIM`: AOSP tints its (white) badge
  drawable to the notification's background colour so the rim reads as a
  gap, but a glass card has no one colour to borrow, and near-white
  separates the glyph from a light avatar and a dark one alike. A card with
  no content image leads with its identity and wears no badge. There is no
  second icon: one column says both who sent it and which app carried it.

## Actions

- Non-`default` actions render as a clickable button row; a left click emits
  `ActionInvoked` and dismisses the card unless the `resident` hint holds it.
  Under the `action-icons` hint each action id is a freedesktop icon name
  drawn on the button.
- The `default` action (and a lone action) fire on a body left click, on
  BOTH surfaces, and are never drawn as a button. The spec defines it as
  "the default action (usually invoked by clicking the notification)" and
  says implementations are free not to display it; Material Design likewise
  says action buttons must not duplicate the tap action.
  `ActivationToken` precedes each invoke (a compositor-minted
  xdg-activation token) so the sender can raise itself.

## Behavior

- Popup clicks: left invokes the action / opens the link / fires the default,
  then dismisses; right dismisses; middle parks the stack into the shade. The
  cards own the pointer over them — hover never leaks to the window beneath
  (sloppy focus would flip focus under every popup).
- Popup hover HOLDS the timeout: the card under the pointer stops counting
  down, and leaving restarts its full clock rather than resuming the sliver
  that was left (Android's heads-up does the same when a touch ends). Only
  one card can be held, because only one can be hovered.
- Shade clicks: a row behaves as its banner did — left on the body fires the
  card's primary and dismisses unless `resident`, a link opens, a button
  acts. Rows open by default, so the click is spent acting rather than
  revealing; the CHEVRON is the only fold target. Right dismisses; middle is
  "Clear all". On an app bundle left expands and right (or the header ✕)
  dismisses the whole app.
- Acting CLOSES the shade. Firing a card's primary, pressing one of its
  buttons or opening a body link all raise something over the panel the
  click was made in, so the panel gets out of the way. This is Android's
  rule, split the same way: `StatusBarNotificationActivityStarter`
  collapses the shade on a content-intent click, and `handleRemoteViewClick`
  closes it for any action that starts an activity — but an action that
  does not start one leaves the shade standing. fd.o has no `isActivity`,
  and `resident` is the nearest thing it does have ("the server will not
  automatically remove the notification when an action has been invoked"),
  so the shade goes exactly when the card goes. Everything that keeps you
  here keeps the shade: a dismissal, a fold, the manage panel, DND, "Clear
  all", the reply field, and a card with no action to fire (that click is
  only a dismissal). swaync draws the same line with `hide-on-action`
  (default on) versus `hide-on-clear` (default off).
- Inline reply (KDE's protocol, which Telegram Desktop speaks): an action
  keyed `inline-reply` is not a button — it grows a reply field in the open
  shade row, and sending emits `NotificationReplied(id, text)` and closes
  the card unless `resident`. `x-kde-reply-placeholder-text` and
  `x-kde-reply-submit-button-text` are honored. The field takes the whole
  keyboard while armed (there is no focus to give it); editing is
  append-and-backspace plus C-u / C-w. Banners have no field.
- Shade keys, while it is open and only then: Esc peels (an open manage panel
  first, then the shade), ↑/↓ move a selection (an accent hairline; the page
  follows it), Space folds, Enter fires the primary, Tab arms the selected
  card's reply field, Delete dismisses, `m` silences the app, `s` snoozes the
  card, `p` marks the sender, `u` takes a snooze back while its undo row is
  up. An undo row is its own keyboard surface: `s` there re-picks the
  duration, and Space/Enter belong to focus since the row has neither a fold
  nor a primary. Modified chords pass through as user binds, and so does any
  key with nothing to act on — nothing selected, or `p` on a card that is not
  a chat.
- Per-app rules (`policy.cpp`, persisted to
  `$XDG_STATE_HOME/hyprnotify/policy.tsv`): SILENCED apps get no banner and
  no sound and rank with the quiet ones — Android's "Silent", dunst's
  `skip_display` — while MARKED conversations rank above everything but a
  critical card and wear AOSP's `conversation_icon_badge_ring`, the one it
  ships with `visibility=gone` until you mark someone — drawn at the badge's
  own diameter with the rim's width for a stroke
  (`importance_ring_size`/`importance_ring_stroke_width`), so marking a chat
  recolours that band instead of hanging a second circle outside it and
  making a marked badge bigger than an unmarked one. Critical bypasses a
  silence exactly as it bypasses DND. Set from the row's manage panel or the
  bundle header; both rules are retroactive, so the cards already in the
  shade re-rank under them. Silence keys on the app identity, a mark on app +
  sender: one chat app carries many people. This is state the user typed with
  a click, never a per-app branch in code.

  A silence carries an EXPIRY (`s<TAB>key<TAB>epoch`, 0 = always), so iOS's
  "Mute for 1 Hour" and "Mute for Today" are sayable and permanent stops
  being the only thing a click can mean — it is the choice people regret.
  Wall clock, not steady time: a suspend, a reboot and a week off all count
  against the hour. Expiry is lazy — a lapsed rule is dropped the next time
  anyone asks, which is every arrival and every paint, and nothing has to
  HAPPEN at the moment a silence lifts. A rule that lapsed while the session
  was down never loads. The footer's `⊘ N` is the other half: the shade
  admits what it is holding back, and one click lifts every rule.
- The manage panel: the ⋮ turns a row into its own verbs rather than opening
  a floating menu. Same ergonomics as Android's long-press panel — full-width
  labelled targets, each with its key in the right column — with none of a
  second surface's cost: no z-order, no outside-click grab, no damage region
  of its own, and it rides the fold machinery that already exists. One row
  wears it at a time. Esc peels it before the shade. It replaced three 20px
  glyphs at 4px separation, hover-only and unlabelled, with the irreversible
  verb in the middle slot.
- Snooze (`s` or the panel, `snooze_seconds`, 900): the card goes out of
  sight and comes back alerting — Android's snooze. It stays in the model
  while away, so `state` counts it while the badge does not, and "Clear all"
  leaves it alone. The wake re-keys the arrival spring but not `arrived`: the
  age line still tells the truth about when the card came. Ephemerals are
  refused — expiry takes those cards whole, so there is nothing to come back
  to.

  It does NOT leave at the click, which is what stopped it being the one
  irreversible verb in the shell. Android replaces the notification in place
  with "Snoozed for 1 hour ▾ · Undo" and lets it go afterwards; so does this.
  For CONFIRM_MS (6s) the card holds its slot as a one-line undo row — Undo
  or `u` restores it, the ˅ or `s` cycles Android's 15m/30m/1h/2h ladder, and
  each change re-arms both clocks. Closing the shade commits every pending
  snooze rather than stranding a window nobody can see. Not history and not
  recall: the card never left. ONE event-loop timer serves all three clocks —
  banner expiry, the wake, and the undo window — since all are deadlines on
  the same list.
- Swipe: a horizontal wheel on a row, away to dismiss and back to open the
  manage panel. Both go through the CLICK queue rather than acting in the
  emission (crash class 6) — a swipe is an alias for a click that already
  exists. Strictly an addition: a mouse with no horizontal wheel never
  reaches it, so neither gesture may be the only way to reach its verb.
- Native layer-shell precedence: cards and the shade are compositor-drawn,
  so their pointer ownership is subordinate to the fork's fresh native hit
  test for exclusive layers, layer popups, overlay layers, IME popups, and top
  layers, including the native fullscreen filter for top layers that are not
  marked `aboveFullscreen`. Client implicit pointer grabs and native seat grabs
  (`xdg_popup` and focus-grab surfaces) also remain untouched, even with no
  button held. While `hyprland-input-capture-v1` is active, the capture client
  owns buttons, axes, keys, and pointer warps before these plugin surfaces;
  notification input passes through and partial swallow state is reset.
- Quiet while fullscreen (`quiet_fullscreen`, on): a real fullscreen window
  on the focused monitor holds banners back — presenting, gaming and
  watching are the same ask — and the card lands resident in the shade
  instead. Critical bypasses it; a maximized window is not fullscreen.
- OSD overlays: the reserved 9990-9999 band is still replace-in-place and
  ephemeral, never a shade row, bell count, or "Clear all" target. When the
  shade is already open, its active battery, touchpad, brightness, volume, or
  microphone card is drawn above the panel until its normal short timeout;
  feedback therefore cannot disappear behind the surface that was open when
  the key was pressed.
- Critical: urgent-colored frame and progress fill, never expires.
- Sound: `sound-file`/`sound-name` play through a libcanberra player
  (`sound_command`, empty disables); `suppress-sound` mutes one arrival. The
  compositor has no audio backend, so this shells out, reaped off the event
  loop.
- DND (`hl.plugin.hyprnotify.suspend()`): arrivals collect silently with
  timeouts held; resume renders the queue newest-first on fresh timeouts and
  reuses the live banner eligibility policy. Snooze wake does the same, so
  fullscreen quiet, app silence, and one-popup coalescing still apply.
- Residency (`persistence`): an expired banner RETREATS into the shade
  rather than closing, and waits there until dismissed or acted on — the
  shade is the safety net. There is no history and no recall: a dismissed
  card is gone, as on Android. `hyprctl hyprnotify count` answers the live
  total (the lockscreen bell reads it).
- The conversation merge (Android's MessagingStyle): a fresh `Notify` whose
  app identity + summary matches a live card is joined onto it, bodies
  appended under an 8KB cap — so one chat is one growing card however many
  messages arrive. Triggered by the fd.o conversation categories
  (`im.*`/`call.*`, where the summary is the sender or the room) or by the
  `x-canonical-append` hint. Cards that vanish on expiry never merge.
- Fullscreen: while a card is up over a solitary fullscreen window, the
  compositor's `render.preChecks` hook requests a normal monitor render so
  the card composites over it. When the last card clears, the normal render
  path re-latches solitary rendering and scanout.
- Session lock: cards never render above the lockscreen (the built-in
  `hyprctl notify` overlay does; these are the user's notifications). Input
  listeners guard-and-reset first; whatever survives the lock repaints at
  unlock.
- Overflow: `max_notifs` caps the model — overflow evicts the oldest
  non-critical card (critical last) with `NotificationClosed`.

## Limitations

- GIF images don't decode (hyprgraphics has no GIF codec).
- No animated icons (`icon-multi`).
- Icon-theme resolution is a pragmatic scan (GTK theme → hicolor →
  pixmaps), not a full `index.theme` inheritance engine.

## Config

`plugin:hyprnotify:*` — `font`, `font_size` (12), `width` (348),
`max_height` (300), `max_icon` (44), `margin` (6, screen edge +
inter-card), `offset_y` (34, clears the bar), `timeout_low` (4000, the
ephemerals' clock), `timeout_normal` (5000, then the banner retreats to the
shade; 0 = sticky), `coalesce_popups` (1), `rounding`, `rounding_power`,
`max_notifs`, `ignore_dbusclose`, `quiet_fullscreen` (1),
`fallback_icon_dir`, `sound_command`
(`canberra-gtk-play`), `col_bg`, `col_fg`, `col_title`, `col_kicker`,
`col_frame`, `col_urgent`, `col_highlight`, `col_link`. Colors and fonts
arrive from `theme.lua`; the C++ defaults mirror it.
