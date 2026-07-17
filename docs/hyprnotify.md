# hyprnotify details

The compositor owns `org.freedesktop.Notifications`: no external daemon, no
layer surface. Cards render top-right on the focused monitor, oldest at the
top, styled like the old naughty boxes (flat dark cards, 1px frame, big
icons — `max_icon` is the old `naughty` `icon_size = 100`).

## Spec surface

- Methods: `Notify`, `CloseNotification`, `GetCapabilities`,
  `GetServerInformation`. Signals: `NotificationClosed`, `ActionInvoked`.
- Capabilities: `actions`, `body`, `icon-static`.
- `replaces_id` updates a card in place, keeping its stack slot; an unknown
  id creates the card under that id (the OSD scripts pin fixed ids).
- `expire_timeout`: −1 → per-urgency defaults (`timeout_low`,
  `timeout_normal`; critical is sticky), 0 → sticky, >0 → ms.
- Hints: `urgency`; `value` (0–100) draws the progress bar —
  the volume/brightness OSD; `image-data`/`image_data`/`icon_data` raw
  pixmaps; `image-path`/`image_path`.
- Images: `image-data` beats `image-path` beats `app_icon` (the spec's
  precedence). File paths and `file://` URIs only — bare theme icon NAMES
  resolve to nothing, like naughty's path-only `icon`. Decoding is
  hyprgraphics: PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG (no GIF); big images
  downscale once at load, not per frame.
- Markup is never advertised; incoming tags are stripped, entities
  unescaped. The summary renders bold on one ellipsized line; the body
  word-wraps to the card and the tail line ellipsizes at `max_height`.

## Behavior

- Clicks: left invokes the client's `default` action (or its only action),
  then dismisses; right dismisses the card; middle sweeps the whole stack.
  The cards own the pointer over them — hover never leaks to the window
  beneath (sloppy focus would flip focus under every popup).
- Critical: urgent-colored frame and progress fill, never expires.
- Session lock: cards never render above the lockscreen (the built-in
  `hyprctl notify` overlay does; these are the user's notifications).
  Timeouts keep running; whatever survives the lock repaints at unlock.
- Overflow: cards that don't fit below the stack wait off-screen with
  their timeouts running.
- No history, no sound: naughty had neither, and the dunst-era
  `history-pop`/`close-all` binds were dropped with dunst.

## Limitations

- One action per card (the default) — no action buttons, no context menu.
- No xdg-activation token on `ActionInvoked`: a client that wants to focus
  itself in response may be denied by focus stealing prevention.
- Bare theme icon names (`app_icon = "firefox"`) don't resolve; send a
  path, a `file://` URI, or image hints.
- GIF images don't decode (hyprgraphics has no GIF codec).

## Config

`plugin:hyprnotify:*` — `font`, `font_size`, `width` (340), `max_height`
(260), `max_icon` (100), `margin` (4, screen edge + inter-card),
`offset_y` (30, clears the bar), `timeout_low` (4000), `timeout_normal`
(8000), `col_bg`, `col_fg`, `col_frame`, `col_urgent`, `col_highlight`.
Colors and fonts arrive from `theme.lua`; the C++ defaults mirror it.
