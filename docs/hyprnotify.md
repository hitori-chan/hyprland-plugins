# hyprnotify details

The compositor owns `org.freedesktop.Notifications`: no external daemon, no
layer surface. Cards render top-right on the focused monitor, newest at the
top, styled like the old naughty boxes (flat dark cards, 1px frame, big
icons).

## Spec surface

- Methods: `Notify`, `CloseNotification`, `GetCapabilities`,
  `GetServerInformation` (spec 1.3). Signals: `NotificationClosed`,
  `ActionInvoked`, `ActivationToken`.
- Capabilities: `actions`, `action-icons`, `body`, `body-markup`,
  `body-hyperlinks`, `body-images`, `icon-static`, `persistence`, `sound`.
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
  offset); a click opens the URL via `xdg-open` and leaves the card up. The
  pointer shows the hand over a link.
- `<img src>` in the body renders as a thumbnail row below the text.

## Images / icons

- Precedence: `image-data` beats `image-path` beats `app_icon` beats
  `desktop-entry`.
- Each of `app_icon` / `image-path` / `desktop-entry` may be a file path
  (`file://` too) OR a freedesktop icon NAME, resolved against the GTK icon
  theme, then hicolor, then `/usr/share/pixmaps`.
- Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG (no GIF); big
  images downscale once at load, not per frame. Wide images (aspect ≥ 1.5)
  render card-width as a cover-cropped hero. Iconless cards draw a random
  face from `fallback_icon_dir`.

## Actions

- Non-`default` actions render as a clickable button row; a left click emits
  `ActionInvoked` and dismisses the card unless the `resident` hint holds it.
  Under the `action-icons` hint each action id is a freedesktop icon name
  drawn on the button.
- The `default` action (and a lone action) fire on a body left click.
  `ActivationToken` precedes each invoke (a compositor-minted xdg-activation
  token) so the sender can raise itself.

## Behavior

- Clicks: left invokes the action / opens the link / fires the default, then
  dismisses; right dismisses; middle sweeps the visible stack. The cards own
  the pointer over them — hover never leaks to the window beneath (sloppy
  focus would flip focus under every popup).
- Critical: urgent-colored frame and progress fill, never expires.
- Sound: `sound-file`/`sound-name` play through a libcanberra player
  (`sound_command`, empty disables); `suppress-sound` mutes one arrival. The
  compositor has no audio backend, so this shells out, reaped off the event
  loop.
- DND (`hl.plugin.hyprnotify.suspend()`): arrivals collect silently with
  timeouts held; resume renders the queue newest-first on fresh timeouts.
- History (`persistence`): a closed card is retained (`max_history`) unless
  it is `transient` or a progress/OSD card; `hl.plugin.hyprnotify.recall()`
  (or `hyprctl hyprnotify recall`) pops the most recent back with a fresh
  timeout. `hyprctl hyprnotify {count,history}` answer the live and retained
  totals (the lockscreen bell reads `count`).
- Fullscreen: while a card is up over a solitary fullscreen window, the
  monitor's scanout/solitary latch is dropped so the card composites over
  it; self-heals once the last card clears.
- Session lock: cards never render above the lockscreen (the built-in
  `hyprctl notify` overlay does; these are the user's notifications). Input
  listeners guard-and-reset first; whatever survives the lock repaints at
  unlock.
- Overflow: `max_notifs` caps the model — overflow evicts the oldest
  non-critical card (critical last) with `NotificationClosed`.

## Limitations

- GIF images don't decode (hyprgraphics has no GIF codec).
- No animated icons (`icon-multi`) and no inline-reply.
- Icon-theme resolution is a pragmatic scan (GTK theme → hicolor →
  pixmaps), not a full `index.theme` inheritance engine.

## Config

`plugin:hyprnotify:*` — `font`, `font_size` (12), `width` (340),
`max_height` (260), `max_icon` (64), `margin` (4, screen edge + inter-card),
`offset_y` (30, clears the bar), `timeout_low` (4000, the ephemerals'
clock), `timeout_normal` (0 = sticky until dismissed), `rounding` (1),
`max_notifs` (50), `max_history` (20),
`fallback_icon_dir`, `sound_command` (`canberra-gtk-play`), `col_bg`,
`col_fg`, `col_title`, `col_kicker`, `col_frame`, `col_urgent`,
`col_highlight`, `col_link`. Colors and fonts arrive from `theme.lua`; the
C++ defaults mirror it.
