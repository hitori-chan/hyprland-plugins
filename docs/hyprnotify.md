# hyprnotify details

The compositor owns `org.freedesktop.Notifications`: no external daemon, no
layer surface. Two surfaces share one card model — glass banners top-right
on the focused monitor, and the two-view center (the Android shade). The
skin is glass·ink: frosted graphite, live blur, IBM Plex Sans, superellipse
corners (`rounding_power`).

## Spec surface

- Methods: `Notify`, `CloseNotification`, `GetCapabilities`,
  `GetServerInformation` (spec 1.3). Signals: `NotificationClosed`,
  `ActionInvoked`, `ActivationToken`.
- Capabilities: `actions`, `action-icons`, `body`, `body-markup`,
  `body-hyperlinks`, `body-images`, `icon-static`, `persistence`, `sound`.
- `replaces_id` updates a card in place, keeping its stack slot; an unknown
  id creates the card under that id (the OSD family pins fixed ids in the
  9990s, which fresh ids never mint into).
- `expire_timeout`: −1 → per-urgency defaults (`timeout_low`,
  `timeout_normal`; critical is sticky), 0 → sticky, >0 → ms.
- Hints honored: `urgency`; `value` (0–100) draws the progress pill;
  `image-data`/`image_data`/`icon_data` raw pixmaps;
  `image-path`/`image_path`; `desktop-entry`; `x-canonical-append`;
  `action-icons`; `resident`; `transient`;
  `sound-file`/`sound-name`/`suppress-sound`.
- The shell face: `org.hitori.hyprnotify` on the same object carries
  `Toggle` (the center), `State` (method + signal: live, kept, dnd,
  center) — hyprbar's bell rides it; the channel is the bus, never symbols.
  The counts are the shade's: `live` = bannered popups, `kept` = resident
  cards; history, the DND queue and the OSD band never count.

## Residency (the Android model)

- A banner timeout emits `NotificationClosed` reason 1 exactly once and
  hides ONLY the popup: the card stays resident in the shade view until
  dismissed or acted on. History receives dismissed and app-closed entries
  only.
- `transient` cards and progress (OSD) cards vanish entirely on expiry —
  neither shade nor history keeps them.
- `ignore_dbusclose` (dunst's knob) gates only the bus `CloseNotification`
  path: an app revoking its own notification is ignored; user dismissals
  and expiry are untouched.
- DND queues arrivals silently with held timeouts; resume shows the queue
  on fresh timeouts. Critical bypasses DND.

## The center

- Two views: the SHADE (resident live cards, unlabeled; "You're all caught
  up!" empty state) and HISTORY behind the ⏱ button ("No history"). The
  view and every fold reset when the center closes.
- One two-state card everywhere: collapsed = icon column + bold
  "title • age" + the newest body line (+ progress) + a 24Ø chevron;
  expanded = header ("App • age"; group children show the age alone),
  title, 4-line body, progress, and the notification's ORIGINAL actions as
  tinted text buttons — live AND history (a history invoke emits
  `ActionInvoked` with the original id, best effort, and consumes the
  entry). Live arrives expanded and auto-folds when its banner expires; the
  chevron overrides both ways. No hover-✕ in the center — popups keep it.
- Groups: ≥2 same-app rows fold in BOTH views (the OSD band never groups);
  three states — digest (identity icon · "App • N • age" · count pill ·
  ≤2 indented preview lines) → header + segmented 1-line children →
  per-child full expand. Shade children dismiss (reason 2 → history);
  history children recall/delete. A group's ✕ (or right click) takes the
  whole app's entries.
- Input: left = default action (live) / recall (history); right = dismiss /
  delete; middle = sweep live → history / clear history; wheel pages —
  captured only inside the panel box; esc (and an outside click) closes.
  The bottom bar: ⏱ · "Clear all"/"Clear history" (greys when its target
  is empty) · ⊖ DND.

## Icon anatomy

- The CONTENT image (`image-data`, else `image-path`) owns the icon column,
  wearing the IDENTITY (`app_icon`, else `desktop-entry`) as a 13px corner
  badge when both exist and differ. Identity-only cards lead with identity.
  Nothing at all = a text-only card (no fallback art — retired in 4.0.0).
  Group digests/headers lead with identity; children ride plain avatars.
- Each source may be a file path (`file://` too) OR a freedesktop icon
  NAME, resolved by `common/icons.hpp` (GTK theme incl. the KDE
  `<context>/<size>` layout → hicolor → Adwaita → pixmaps).
- Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG (no GIF); big
  images downscale once at load. Wide content (aspect ≥ 1.5) renders as a
  cover-cropped hero on popups.

## Markup

- Body and title render the whitelisted Pango subset: `<b> <i> <u> <span>
  <br>`. Other tags are dropped; a stray `<`/`&` that forms no tag or
  entity survives as literal text. Malformed markup falls back to plain.
- `<a href>` is a hyperlink (rewritten to a styled span, hit-tested by its
  stripped-text byte offset); a click opens the URL via `xdg-open` and
  leaves the card up. Links are popup-only; center rows act, not browse.
- `<img src>` renders as a thumbnail row below the text.

## Model rules

- Append: a fresh Notify with `x-canonical-append` matching a live card's
  app + summary rides the replace path — bodies join under a ~8KB cap
  (oldest lines drop), the timeout refreshes, the slot stays, a fresh icon
  wins. One conversation = one card = one history entry. The collapsed
  line shows the NEWEST message (the last body line).
- The OSD band (ids 9990–9999): replace-in-place, excluded from append
  matching, groups, and history; counts in the bell badge only while
  visible; fresh ids and recalls never mint into it.
- Recall (`hyprctl hyprnotify recall`, a history row's left click) returns
  the entry live under a fresh id with its ORIGINAL arrival time — the age
  line keeps telling the truth.
- Overflow: `max_notifs` evicts the oldest non-critical card (critical
  last) with `NotificationClosed` reason 4; the evicted entry stays
  recallable from history.
- Ages are bucketed ("now", "5m", "2h", "3d") and tick every 30s while
  anything shows.

## Behavior

- `ActivationToken` (a compositor-minted xdg-activation token) precedes
  each invoke so the sender can raise itself.
- Critical: an urgent hairline ring and progress fill, sticky, punches
  through DND.
- Sound: `sound-file`/`sound-name` through `sound_command` (libcanberra;
  empty disables); `suppress-sound` mutes one arrival.
- Fullscreen: while anything shows over a solitary fullscreen window, the
  monitor's scanout/solitary latch is dropped so it composites; self-heals.
- Session lock: nothing renders above the lockscreen; input listeners
  guard-and-reset there first.
- Motion: the arrival/open springs damage only their boxes and honor
  `animations=0` as the kill switch.

## hyprctl / Lua

`hyprctl hyprnotify {count,history,recall,center,state,clear}` — `state`
prints `center:N live:N hist:N dnd:N`; `clear` dismisses the shade and
wipes history. `hl.plugin.hyprnotify.{suspend,recall,center}()` — F12 is
the reserved center bind.

## Limitations (inherent fd.o deltas vs Android — disclosed, not fixable)

- No typed templates (MessagingStyle): conversation rendering is inferred
  from hints; sender names live inside body text.
- No inline reply (x-kde-reply is a flagged later minor) and no
  channels/importance beyond urgency.
- History actions are best-effort: the original-id invoke can silently
  no-op once the app forgot the id.
- Grouping keys on app identity — apps that rename per window can split
  groups.
- GIF doesn't decode; icon resolution is a pragmatic scan, not a full
  `index.theme` engine.

## Config

`plugin:hyprnotify:*` — `font` (IBM Plex Sans), `font_size` (12; the type
roles derive from it), `width` (348), `max_height` (300), `max_icon` (44),
`margin` (6), `offset_y` (34), `timeout_low` (4000), `timeout_normal`
(8000), `rounding` (16; the panel is +6, rows −2), `rounding_power` (3.0),
`max_notifs` (50), `max_history` (20), `ignore_dbusclose` (0),
`sound_command` (`canberra-gtk-play`), `col_bg` (the glass — alpha is the
frost), `col_fg`, `col_title`, `col_kicker`, `col_frame`, `col_urgent`,
`col_highlight` (the accent), `col_link`. The C++ defaults ARE the
glass·ink tokens; theme.lua overrides them.
