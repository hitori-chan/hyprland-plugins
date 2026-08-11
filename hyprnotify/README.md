# hyprnotify

A compositor-native `org.freedesktop.Notifications` 1.3 daemon with Pixel/AOSP
inspired banners and a pointer-driven notification center. It replaces dunst,
mako, or another notification daemon and creates no helper window.

It supports actions, action icons, bounded markup and links, images, progress,
sound, persistence, replacement, and KDE inline reply. Application identity
comes from `app_icon` or the sender's desktop entry; unresolved applications use
one deterministic generic mark.

## Banners

- Cards appear at the focused monitor's top-right. Left click invokes a link or
  default action, right click dismisses, and middle click moves the stack into
  the center.
- Normal cards retreat into the center after `timeout_normal`. Low, transient,
  and progress cards expire after `timeout_low`; critical cards stay until
  dismissed. Hover pauses the active timeout.
- `coalesce_popups` admits one non-critical banner per application while keeping
  the rest in the center. `quiet_fullscreen` is opt-in; by default ordinary and
  critical banners remain visible over fullscreen applications.
- Conversations use one leading avatar plus a small application badge. Group
  conversations can use a two-person face pile. Ordinary notifications show one
  unbadged application icon.

## Notification Center

The bar bell or `hyprctl hyprnotify center` toggles the live list. Opening the
center absorbs current banners. Dismissed cards are gone; there is no history,
recall, center keyboard shortcut, keyboard navigation, chevron, kebab, popup
close button, or normal-row Snooze button.

- Cards rank by critical, priority conversation, conversation, normal, then
  silent, newest first within a tier.
- Two eligible cards sharing an application/group key form a bundle. Explicit
  app groups override automatic application/section grouping; conversations
  remain distinct children.
- The count pill is the only expansion control. Standard groups expose at most
  2, 5, or 8 children in collapsed, system-expanded, or user-expanded state;
  classified groups use 0, 30, and 50. Wheel input pages the remaining rows.
- Body, link, action, and right-click behavior matches banners. Acting closes
  the center unless a resident notification remains. Clicking outside closes
  the center.
- Hold a row for 500 ms, or use the horizontal back gesture, to manage it.
  Conversation rows stage Priority, Default, or Silent; other rows stage
  Default or Silent. Done commits, Dismiss removes, and right-click/back cancels.
- Eligible singleton management panels also expose Snooze. Snoozing keeps the
  card in the model, shows a six-second Undo row, then returns it alerting after
  `snooze_seconds`. Bundles do not expose Snooze.
- Silent is application-wide; Priority targets one stable conversation. Policy
  persists in `$XDG_STATE_HOME/hyprnotify/policy.tsv`.
- Reply is the only keyboard-owning center state. A visible Reply action arms a
  bounded one-line UTF-8 editor with navigation, selection, word editing, and
  asynchronous `C-v`. Enter/Send submits, Esc cancels, and an empty submit keeps
  the field open. The draft limit is 2,000 bytes. IME/preedit is not supported.
- The footer contains compact DND and policy-reset controls plus Clear all.
  Clear all excludes DND-queued, snoozed, and private OSD cards.

## OSD And Control

IDs 9990-9999 are private transient OSD cards. Battery, touchpad, brightness,
volume, and microphone feedback replace in place, stay outside center ranking
and Clear all, and render below an open center. They use the same card surface
as banners with stable native semantic identities.

```text
hyprctl hyprnotify {count,center,state,badge,policy,snoozed,clear}
hl.plugin.hyprnotify.suspend()
```

The optional structured-conversation hints, admission limits, identity rules,
and compositor contracts are in
[`docs/hyprnotify.md`](../docs/hyprnotify.md).

## Config

Opaque defaults avoid blur work. Giving a panel/card color an alpha below 1
uses the target fork's rounded glass path.

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprnotify:font` | font family | `Roboto` |
| `plugin:hyprnotify:font_size` | body text size | 12 |
| `plugin:hyprnotify:width` | popup width | 380 |
| `plugin:hyprnotify:max_height` | popup height cap | 300 |
| `plugin:hyprnotify:max_icon` | popup identity cell | 40 |
| `plugin:hyprnotify:margin` | inter-card gap | 8 |
| `plugin:hyprnotify:offset_y` | distance from monitor top | 34 |
| `plugin:hyprnotify:timeout_low` | ephemeral timeout in ms | 4000 |
| `plugin:hyprnotify:timeout_normal` | normal timeout; 0 is sticky | 5000 |
| `plugin:hyprnotify:quiet_fullscreen` | suppress non-critical fullscreen banners | 0 |
| `plugin:hyprnotify:snooze_seconds` | snooze duration | 900 |
| `plugin:hyprnotify:coalesce_popups` | one non-critical popup per app | 1 |
| `plugin:hyprnotify:max_notifs` | model cap | 50 |
| `plugin:hyprnotify:ignore_dbusclose` | ignore app `CloseNotification` calls | 0 |
| `plugin:hyprnotify:rounding` | outer card radius | 28 |
| `plugin:hyprnotify:rounding_power` | corner exponent | 2.0 |
| `plugin:hyprnotify:sound_command` | sound player; empty disables | `canberra-gtk-play` |
| `plugin:hyprnotify:col_bg` | shade/panel | `ff132732` |
| `plugin:hyprnotify:col_surface` | card/row | `ff172025` |
| `plugin:hyprnotify:col_surface_high` | raised controls | `ff243944` |
| `plugin:hyprnotify:col_state` | hover/selected state | `339acbff` |
| `plugin:hyprnotify:col_fg` | body text | `ffeef3f5` |
| `plugin:hyprnotify:col_title` | title | `fff3f6f7` |
| `plugin:hyprnotify:col_kicker` | metadata | `ffd1dde1` |
| `plugin:hyprnotify:col_frame` | outline | `33e0f0f8` |
| `plugin:hyprnotify:col_urgent` | critical content | `ffffb4ab` |
| `plugin:hyprnotify:col_highlight` | actions/progress | `ff9acbff` |
| `plugin:hyprnotify:col_on_highlight` | content on primary | `ff102333` |
| `plugin:hyprnotify:col_on_urgent` | content on error | `ff690005` |
| `plugin:hyprnotify:col_link` | links | `ff9acbff` |

These are fixed semantic defaults, not wallpaper-derived dynamic color. Every
role remains independently configurable.
