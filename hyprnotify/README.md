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
center absorbs current banners. Cards follow the ROM recipe: a semibold
title/who with a grey age tail on one line beside the avatar, the body below in
the larger type, and a circular chevron (or the count pill) to expand.

Dismissed cards land in the history sheet: the footer's history pill, or a
>90px horizontal flick anywhere on the shade, flips to the last 32
dismissals; Clear empties the list, not the model. There is still no center
keyboard shortcut or keyboard navigation, no kebab, popup close button, or
per-row Snooze button — snooze lives in the hold menu.

- Cards rank by critical, priority conversation, conversation, normal, then
  silent, newest first within a tier.
- Declared app groups fold into one bundle at two cards; automatic
  application/section groups form at four (AOSP). Conversations remain
  distinct children. Expanded bundle children show a 24dp circular sender
  icon — conversation participant art, the notification's own content image
  when it is sender-specific, or a deterministic generated avatar — 16dp
  before the text.
- Expansion is the count pill on counted cards or the circular chevron on
  everything else. Standard groups expose at most 2, 5, or 8 children in
  collapsed, system-expanded, or user-expanded state; classified groups use
  0, 30, and 50. Wheel input pages the remaining rows.
- Body, link, action, and right-click behavior matches banners. Acting closes
  the center unless a resident notification remains. Clicking outside closes
  the center.
- Hold a row for 500 ms, or use the horizontal back gesture, to manage it.
  Conversation rows stage Priority, Default, or Silent; other rows stage
  Default or Silent. Done commits, Dismiss removes, and right-click/back cancels.
- Eligible singleton management panels also expose Snooze. Snoozing keeps the
  card in the model, shows a six-second Undo row, then returns it alerting
  after the option chosen in the menu (AOSP's fixed 15 min / 30 min / 1 h /
  2 h list). Bundles do not expose Snooze.
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

The default material is `tray`: the hyprbar tray icon's right-click menu
palette, baked **opaque** — the #0f1218 island with white @6%/11% surface
layers, the #32d6ff accent, and #dcebff hairlines — so the shade reads over
any wallpaper without a blur pass behind it. `theme = "ink"` switches every
role the user never overrode to the v13 AOSP set (the captures' own opaque
near-black #0f1114/#1b1b1e, no rims) and `theme = "glass"` to the tray-menu
frost (translucent surfaces with the compositor's live blur behind them,
`decoration:blur`); an explicit color always wins over any set.

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprnotify:theme` | material set: `tray` (the hyprbar tray-menu palette, opaque), `ink` (the v13 AOSP palette) or `glass` (the frost) | `tray` |
| `plugin:hyprnotify:font` | font family | `IBM Plex Sans` |
| `plugin:hyprnotify:font_size` | body text size in logical px (type roles derive from it) | 12 |
| `plugin:hyprnotify:width` | popup width | 380 |
| `plugin:hyprnotify:max_height` | popup height cap | 300 |
| `plugin:hyprnotify:max_icon` | popup identity cell | 40 |
| `plugin:hyprnotify:margin` | inter-card gap | 8 |
| `plugin:hyprnotify:offset_y` | distance from monitor top (the 26px bar) | 26 |
| `plugin:hyprnotify:timeout_low` | ephemeral timeout in ms | 4000 |
| `plugin:hyprnotify:timeout_normal` | normal timeout; 0 is sticky | 5000 |
| `plugin:hyprnotify:quiet_fullscreen` | suppress non-critical fullscreen banners | 0 |
| `plugin:hyprnotify:snooze_seconds` | how long a snoozed card stays out of sight before it alerts again | 900 |
| `plugin:hyprnotify:coalesce_popups` | one non-critical popup per app | 1 |
| `plugin:hyprnotify:max_notifs` | model cap | 50 |
| `plugin:hyprnotify:ignore_dbusclose` | ignore app `CloseNotification` calls | 0 |
| `plugin:hyprnotify:rounding` | outer card radius | 28 |
| `plugin:hyprnotify:rounding_power` | corner exponent | 2.0 |
| `plugin:hyprnotify:sound_command` | sound player; empty disables | `canberra-gtk-play` |
| `plugin:hyprnotify:col_bg` | shade/panel fill | `ff0f1114` |
| `plugin:hyprnotify:col_surface` | card/row fill | `ff1b1b1e` |
| `plugin:hyprnotify:col_surface_high` | raised controls | `1cffffff` |
| `plugin:hyprnotify:col_state` | hover/selected state | `2bffffff` |
| `plugin:hyprnotify:col_fg` | body text | `ffe8ecf2` |
| `plugin:hyprnotify:col_title` | title | `ffe8ecf2` |
| `plugin:hyprnotify:col_kicker` | metadata | `d1e8ecf2` |
| `plugin:hyprnotify:col_frame` | outline | `00000000` (none) |
| `plugin:hyprnotify:col_urgent` | critical content | `ffff8a5c` |
| `plugin:hyprnotify:col_highlight` | actions/progress | `ffa8c7fa` |
| `plugin:hyprnotify:col_on_highlight` | content on primary | `ff06222e` |
| `plugin:hyprnotify:col_on_urgent` | content on error | `ff07161c` |
| `plugin:hyprnotify:col_link` | links | `ffa8c7fa` |

These are fixed semantic defaults, not wallpaper-derived dynamic color. Every
role remains independently configurable; the color roles' registered
defaults are the INK set's values, and while a role still holds its
default it follows the active material set instead.
