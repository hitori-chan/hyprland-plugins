# hyprnotify

A compositor-native `org.freedesktop.Notifications` 1.3 daemon with Android
inspired banners and notification center. It replaces external daemons such as
dunst or mako and renders no layer surface or helper window.

Supported capabilities include actions, action icons, markup, hyperlinks, body
images, inline reply, persistence, and sound. Application identity comes from
`app_icon` or the sender's desktop entry; missing identity uses one deterministic
generic app mark. Content images never become random application badges.

## Banners

- Cards appear at the focused monitor's top-right with app identity, age,
  title, body, progress, image, and actions as available.
- Conversation notifications may use the sender avatar plus application badge;
  ordinary notifications keep the application identity on the left and show a
  separate content preview on the right. Wide images become full-width hero
  media.
- Hovering a card restarts its timeout when the pointer leaves. Normal banners
  retreat into the center after `timeout_normal`; critical cards stay
  until dismissed. Transient, low-urgency, and progress cards use
  `timeout_low` and disappear completely.
- `coalesce_popups` limits an app to one non-critical banner. Extra cards remain
  resident in the center. `quiet_fullscreen` applies the same treatment while a
  real fullscreen window owns the monitor. Critical cards bypass both, app
  silence, and DND.
- Left click invokes a link/default action, right click dismisses, and middle
  click parks the banner stack in the center.

## Notification Center

The bar bell or `hyprctl hyprnotify center` toggles one list of live cards.
Opening absorbs visible banners; dismissed cards are gone and there is no
history or recall. The center has no keyboard shortcut, Lua action, keyboard
navigation, chevron, or kebab menu.

- Ranking is critical, marked conversations, other conversations, normal, then
  silent; newest first within each tier.
- Four or more cards from one app form a bundle. Conversations remain separate
  and matching conversation arrivals merge into one growing card.
- Rows open while the panel has room. A compact row expands from its body before
  any hidden action can fire. Wide content uses the same full-width hero media
  anatomy as banners once the row is open. Wheel paging handles remaining rows.
- Once open, body click invokes the default action when present and otherwise
  dismisses; links and buttons act, right-click dismisses, and an outside click
  closes the center. Acting closes it unless `resident` keeps the card in place.
- Hold a row body for 500 ms, or use the horizontal back gesture, to open its
  full-width management panel. Management never uses a separate menu button.
- Mute supports one hour, today, or always. Priority marks one conversation.
  Rules persist under `$XDG_STATE_HOME/hyprnotify/policy.tsv`; critical cards
  still bypass silence.
- Snooze replaces the row with a six-second Undo state. Its duration control
  cycles 15m, 30m, 1h, and 2h; `snooze_seconds` supplies the first panel value.
  Snoozed cards remain in the model and return alerting.
- An open row with an `inline-reply` action shows a Reply chip. Clicking it arms
  the only keyboard-owning surface in the center: Enter sends, Esc cancels,
  Backspace edits, and `C-u`/`C-w` erase.
- The footer owns compact DND, muted-rule reset, and Clear all controls.

## OSD And Control

Reserved IDs 9990-9999 are private transient OSD cards. Battery, touchpad,
brightness, volume, and microphone feedback cards replace in place, stay
outside center ranking, badges, and Clear all, and render below an open center.
The in-tree names select stable native AOSP/Android semantic marks rather than
theme fallback artwork, including the battery identity used by hyprbar alerts.

```text
hyprctl hyprnotify {count,center,state,badge,policy,snoozed,clear}
hl.plugin.hyprnotify.suspend()
```

Protocol, admission limits, image precedence, lifecycle, and compositor input
contracts are documented in [docs/hyprnotify.md](../docs/hyprnotify.md).

## Config

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprnotify:font` | font family | `IBM Plex Sans` |
| `plugin:hyprnotify:font_size` | body text size | 12 |
| `plugin:hyprnotify:width` | popup width | 348 |
| `plugin:hyprnotify:max_height` | popup height cap | 300 |
| `plugin:hyprnotify:max_icon` | popup icon cap | 44 |
| `plugin:hyprnotify:margin` | screen/card gap | 6 |
| `plugin:hyprnotify:offset_y` | distance from monitor top | 34 |
| `plugin:hyprnotify:timeout_low` | ephemeral timeout in ms | 4000 |
| `plugin:hyprnotify:timeout_normal` | normal banner timeout; 0 is sticky | 5000 |
| `plugin:hyprnotify:quiet_fullscreen` | suppress non-critical fullscreen banners | 1 |
| `plugin:hyprnotify:snooze_seconds` | first snooze duration | 900 |
| `plugin:hyprnotify:coalesce_popups` | one live non-critical popup per app | 1 |
| `plugin:hyprnotify:max_notifs` | model cap | 50 |
| `plugin:hyprnotify:ignore_dbusclose` | ignore app `CloseNotification` calls | 0 |
| `plugin:hyprnotify:rounding` | card radius | 16 |
| `plugin:hyprnotify:rounding_power` | corner superellipse exponent | 3.0 |
| `plugin:hyprnotify:sound_command` | player for sound hints; empty disables | `canberra-gtk-play` |
| `plugin:hyprnotify:col_bg` | glass fill | `9e0f1218` |
| `plugin:hyprnotify:col_fg` | body text | `e4e8ee` |
| `plugin:hyprnotify:col_title` | titles | `eef1f5` |
| `plugin:hyprnotify:col_kicker` | header/age/secondary text | `98a2ac` |
| `plugin:hyprnotify:col_frame` | hairlines | `17dcebff` |
| `plugin:hyprnotify:col_urgent` | critical/progress color | `ff8a5c` |
| `plugin:hyprnotify:col_highlight` | actions/selections | `32d6ff` |
| `plugin:hyprnotify:col_link` | hyperlinks | `7db4ff` |

Colors, fonts, and metrics normally come from `theme.lua`; these are the C++
defaults.
