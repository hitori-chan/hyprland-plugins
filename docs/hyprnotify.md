# hyprnotify contracts

User-visible behavior and configuration live in
[`hyprnotify/README.md`](../hyprnotify/README.md). This document records the
protocol, admission, model, rendering, and compositor contracts.

## Protocol Surface

- Methods: `Notify`, `CloseNotification`, `GetCapabilities`, and
  `GetServerInformation` for specification 1.3.
- Signals: `NotificationClosed`, `ActionInvoked`, `ActivationToken`, and
  `NotificationReplied`.
- Hints: urgency, value, raw/file images, desktop entry, action icons,
  resident, transient, sound, and suppress-sound.
- `replaces_id` updates a card in place. Unknown nonzero IDs create that card;
  ordinary generated IDs never enter the private 9990-9999 OSD band.
- `expire_timeout` 0 is sticky and positive values are milliseconds. Server
  default uses the configured normal/ephemeral policy; critical stays sticky.
- Only non-`default` actions render as buttons. Body click invokes an explicit
  `default` action and otherwise dismisses. `ActivationToken` precedes
  invocation so the sender can raise itself.

Freedesktop sources:

- https://specifications.freedesktop.org/notification/latest/
- https://specifications.freedesktop.org/notification/latest/hints.html
- https://specifications.freedesktop.org/notification/latest/icons-and-images.html

## Admission Bounds

Externally sized state is bounded before it reaches render or input paths:

| Input | Bound |
|---|---|
| application name | 256 bytes |
| summary | 2 KiB |
| body | 8 KiB |
| display hints/action labels | 1 KiB |
| opaque icon/path/action sources | 4 KiB |
| action pairs | first 12 |
| body thumbnails | first 4 |
| markup tag | 1 KiB |
| desktop files indexed | 4,096 |
| desktop entries visited | 16,384 |
| pending image sources | 24 |
| file-backed image | 32 MiB |
| decoded upload | 16 MP |
| model cards | `max_notifs` |

Text truncates only at UTF-8 boundaries. Oversized opaque sources are rejected
rather than reinterpreted. Markup and image scans are single-pass over bounded
input.

The accepted Pango subset is `<b>`, `<i>`, `<u>`, and `<br>`, plus body-only
`<a href>`. Unsupported tags/attributes are removed; malformed markup falls
back to plain text. Links are rewritten to styled spans, hit-tested against
stripped-text offsets, and opened through bounded detached `xdg-open` helpers.
Body `<img src>` entries become thumbnail rows.

## Identity And Media

Content and application identity are separate:

1. `image-data` then `image-path` chooses content/avatar media.
2. `app_icon` chooses application identity.
3. If unresolved, `desktop-entry` is indexed, its `Icon=` value is read, and
   that value is resolved.
4. Missing identity uses one deterministic generic app mark.

Resolution covers file paths, `file://`, the active GTK theme, hicolor,
pixmaps, and Adwaita's symbolic context layout. SVG sources receive an explicit
bounded icon viewport before the asynchronous graphics decode; this is required
by the target hyprgraphics path loader. It is intentionally not a full
`index.theme` inheritance engine.

File-backed PNG, JPEG, WEBP, BMP, AVIF, JXL, and SVG decode asynchronously
through Hyprland's resource gatherer. GIF is unsupported. Completion returns to
the event loop and the decoded surface becomes a texture only during a later
warm pass. Raw image data downscales directly into its bounded output buffer.

Ordinary cards keep application identity in the left icon column. A distinct
non-wide `image-data`/`image-path` source renders as a separate right-side
preview on both popup cards and singleton center rows; it is never promoted to
application identity. Wide content uses the dedicated full-width hero layout.
Conversation cards may instead use Android's 40dp-avatar/20dp-badge container,
with a 16dp app glyph and 2dp rim. Bundle children use their own content as a
small lead when available because the group header already owns app identity.
The Freedesktop `im.*`/`call.*` category classifier is a Linux approximation
of Android conversation metadata, not a claim of shortcut/person parity.

## Model And Lifecycle

One model backs banners and center rows. State transitions preserve stable IDs
and slots:

- Normal expiry retreats a card into center residency.
- Transient/progress expiry removes the card.
- DND, fullscreen quiet, app silence, and popup coalescing suppress banners but
  retain eligible cards.
- Snooze retains a hidden model entry, an undo deadline, and a wake deadline.
- Center opening absorbs banners without dismissing them.
- Overflow evicts the oldest non-critical card before critical entries.
- Conversation merge joins matching app identity plus summary under an 8 KiB
  body cap. App bundling begins at four non-conversation cards.

One event-loop timer owns banner expiry, snooze wake, and undo deadlines. Model,
policy, reply, process, and menu callbacks carry generation or ownership checks
so late work cannot mutate replaced or destroyed state.

`ignore_dbusclose` affects only the bus `CloseNotification` path. User actions,
expiry, Clear all, and overflow retain their normal close semantics.

Policy state is atomically stored in
`$XDG_STATE_HOME/hyprnotify/policy.tsv`. Silence keys on application identity;
priority keys on application plus sender. Timed silence uses wall-clock expiry
so suspend and relog consume the requested interval.

## Rendering And Input

Cards, the center, and OSDs use stable geometry, damage/scissor, and the shared
warm/draw texture gate. A texture is never painted in the frame that creates
it. Hover damage does not rewarm. OSD semantic identity, including battery,
participates in the fixed-ID texture key, preventing replacement from retaining
a stale icon.

Before claiming input, the plugin rechecks native exclusive layers, popups,
overlays, IME surfaces, top layers, client implicit grabs, native seat grabs,
session lock, and active input capture. Native ownership wins. All partial
swallow, reply, drag, hold, and gesture state is reset when lock or ownership
changes.

Long-press and horizontal gestures enqueue the same deferred verbs used by
pointer clicks; they never mutate the model during input emission. Inline reply
owns keys only after a visible Reply chip is clicked. Center paging and all
other notification-center actions are pointer-driven.

While cards are visible over solitary fullscreen, the fork's public full-render
request path exits scanout long enough to composite them. Clearing the final
card allows normal scanout eligibility to return. Cards never render above the
session lock.

## Bus And Process Safety

D-Bus is event-loop integrated and asynchronous. Input/render send sites post
to bounded idle queues; no connection drains inline. Teardown removes event
sources, invalidates callbacks, drops objects/proxies, and then destroys the
connection.

Link and sound helpers have fixed admission limits. Their descriptors and
callbacks are released without waiting for a stuck process; compositor child
reaping owns later exit. Image decoding, desktop indexing, and other blocking
filesystem work stay off compositor dispatch.

## Limits

- No GIF or animated `icon-multi` support.
- Icon-theme lookup is pragmatic rather than a complete inheritance engine.
- The center deliberately has no history/recall or keyboard navigation.
