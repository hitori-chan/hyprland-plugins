# hyprnotify contracts

User behavior and configuration live in
[`hyprnotify/README.md`](../hyprnotify/README.md). This file defines protocol,
admission, model, renderer, input, and lifecycle contracts.

## Protocol

The daemon implements Freedesktop Notifications 1.3 methods and emits
`NotificationClosed`, `ActionInvoked`, `ActivationToken`, and
`NotificationReplied`. It accepts standard urgency, value, image, desktop-entry,
action-icon, resident, transient, sound, suppress-sound, and KDE reply hints.

- A known `replaces_id` updates one daemon record. Unknown IDs create a new
  record outside the private 9990-9999 OSD range.
- Timeout 0 is sticky; positive values are milliseconds. A server-default
  timeout uses normal or ephemeral policy. Critical cards remain sticky.
- Only non-`default` actions render as buttons. Body click invokes an explicit
  default action and otherwise dismisses. Activation token delivery precedes
  action invocation.

Specification: <https://specifications.freedesktop.org/notification/latest/>

### Structured Conversations

The `Notify` signature is unchanged. A sender may add bounded typed hints:

| Hint | Type | Meaning |
|---|---|---|
| `x-hyprnotify-conversation-id` | string | stable chat ID within the app |
| `x-hyprnotify-conversation-title` | string | chat display title |
| `x-hyprnotify-conversation-kind` | string | `one-to-one` or `group` |
| `x-hyprnotify-conversation-icon` | string | chat/shortcut artwork |
| `x-hyprnotify-sender-id` | string | stable participant ID |
| `x-hyprnotify-sender-name` | string | participant display name |
| `x-hyprnotify-sender-icon` | string | participant artwork |
| `x-hyprnotify-message-id` | string | stable message ID |
| `x-hyprnotify-message-timestamp` | int64 | Unix milliseconds |
| `x-hyprnotify-message-historic` | bool | historic message marker |
| `x-hyprnotify-unread-count` | uint32 | count-pill value |
| `x-hyprnotify-group-key` | string | app-declared group |
| `x-hyprnotify-section` | string | automatic-grouping section |

Malformed fields are ignored independently. Omitted fields preserve existing
conversation metadata. `im.*` and `call.*` categories affect presentation but
never authorize text-derived merging.

## Admission

| Input/state | Bound |
|---|---|
| application name | 256 bytes |
| summary / body | 2 KiB / 8 KiB |
| display label / opaque source | 1 KiB / 4 KiB |
| action pairs / body thumbnails | 12 / 4 |
| markup tag | 1 KiB |
| desktop files indexed / visited | 4,096 / 16,384 |
| pending image sources | 24 |
| file-backed image / decoded upload | 32 MiB / 16 MP |
| reply draft | 2,000 bytes |
| conversation field | 512 bytes |
| messages / participants / unread display | 32 / 16 / 999 |
| cards | `max_notifs` |

Desktop-entry identity lookup uses the same bounded event-loop file-index
helper as the launcher. Its private process group is terminated during plugin
exit; no filesystem operation is joined from compositor teardown.

Text truncates at UTF-8 boundaries. Oversized opaque inputs are rejected. The
accepted Pango subset is `<b>`, `<i>`, `<u>`, `<br>`, and body-only `<a href>`;
other markup is stripped and malformed markup falls back to plain text.

## Identity And Media

Application identity and content are separate. Resolution accepts bounded file
paths/URIs, the active GTK theme, hicolor, pixmaps, and Adwaita symbolic
contexts. It is not a complete `index.theme` inheritance engine. PNG, JPEG,
WEBP, BMP, AVIF, JXL, and SVG decode asynchronously; GIF is unsupported.

Every card has one leading identity:

- Ordinary cards use one unbadged application icon or the generic app mark.
- One-to-one conversations select supplied conversation art, latest participant
  art, conversation image media, then a deterministic generated avatar.
- Group conversations select supplied group art or a bounded two-person face
  pile. Missing participant art uses deterministic initials and color.
- Conversation avatars use a 40dp cell with a 20dp application badge. Expanded
  group-conversation sender runs may show one 24dp participant avatar.

Decoded surfaces become textures only during a later warm pass. Raw images are
downscaled directly into bounded output buffers.

## Model

One bounded model backs banners and center rows.

- Normal expiry moves a card to center residency; transient/progress expiry
  removes it. DND, app silence, popup coalescing, and opt-in fullscreen quieting
  suppress banners without losing eligible cards.
- Snooze stores hidden state plus undo/wake deadlines. Opening the center absorbs
  banners. Overflow evicts the oldest non-critical card first.
- Replacement targets one daemon ID. Conversation merge requires exact
  `(appKey, conversationId)` identity; visible text is never a key.
- Message ID replacement preserves omitted metadata. Messages sort by supplied
  timestamp. Pruning removes oldest historic entries before current entries;
  presentation uses the newest seven non-empty messages.
- Explicit group keys override automatic `(appKey, section)` grouping at two.
  Valid sections are `promotions`, `social`, `news`, `recommendations`,
  `alerting`, and `silent`.
- Standard group limits are 2/5/8; classified limits are 0/30/50. Viewport
  clipping can show fewer without changing expansion state.

One event-loop timer owns expiry, snooze, and undo deadlines. Deferred model,
policy, reply, process, and menu work carries generation or ownership checks.
Policy writes atomically to `$XDG_STATE_HOME/hyprnotify/policy.tsv`.

`ignore_dbusclose` affects only application `CloseNotification` requests. User
actions, expiry, overflow, and Clear all retain normal close semantics. Clear all
excludes private OSD, DND-queued, and snoozed cards.

## Rendering And Input

Cards use stable logical geometry, damage/scissor, and the shared warm/draw
texture gate. Opaque defaults skip blur. Translucent colors use the exact fork's
rounded custom-UV blur path so the sampled blur and card share corners.

Core Pixel geometry is 16dp screen inset, 28dp card radius, 32dp center radius,
4dp connected-child radius, 40dp identity, 72dp content start, 60dp end reserve,
72dp minimum row, 48dp actions, 8dp card gap, and a 26x18dp count pill with a
40dp pointer target. Monitor scale applies once after logical layout.

Before taking input, the plugin rechecks session lock, native layers/popups/IME
surfaces, seat and implicit grabs, and input-capture ownership. Native ownership
wins and clears partial swallow, reply, drag, hold, and gesture state. Pointer
mutations are deferred out of input emission.

Reply owns keys only while its visible field is armed. It tracks UTF-8 cursor and
selection offsets, clips/scrolls one line, and uses a cancellable nonblocking
clipboard pipe with a 1.5-second deadline. The exact fork exposes no public
text-input focus contract for a compositor-drawn editor, so IME/preedit remains
unsupported. Every other center action is pointer-driven.

Fullscreen cards request a full render through the fork's public hook and never
draw above session lock. Removing the final card permits normal scanout again.

## Bus And Process Safety

D-Bus is integrated with the compositor event loop. Render/input send sites use
bounded idle queues and never drain a connection inline. Teardown removes event
sources, invalidates callbacks, releases borrowed proxies/objects, then destroys
the connection. Image scans/decodes and process I/O stay off render and input
paths; stuck helpers cannot block plugin teardown.
