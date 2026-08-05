# hyprbar

The AwesomeWM wibar, drawn by the compositor. Renders in each monitor's
reserved top strip (`reserved = { top = 26 }`, matching
`plugin:hyprbar:height`); hides under real fullscreen — except while the
menubar is open, which floats above even that; maximized windows keep it
visible. The strip owns its pointer — hovering it never leaks cursor shape
or focus to a window underneath. The band, the menubar strip and the tray
menus are frosted glass — a translucent `col_bg` over a live blur — when the
compositor's blur is on; the widgets riding them stay opaque.

```
[taglist 一..九] [tasklist of the active workspace ...] [tray] [bell] [battery] [clock] [layoutbox]
```

- **Taglist** — kanji buttons, awesome's state matrix; occupancy as the
  corner square. Clicks and wheel cycles stay on the bar's monitor, while
  `Mod+click` sends the focused window without following it.
- **Tasklist** — the active workspace's windows in arrival order: app icon,
  `⌃` pinned / `+` maximized / `✈` floating markers; minimized windows keep
  their row, muted (awesome's `fg_minimize`). Click the focused task to
  minimize it, click any other (minimized included) to restore + focus;
  right-click opens the all-clients menu, wheel walks focus (skipping
  minimized). `Mod+N` / `Mod+Ctrl+N` minimize / restore-last
  (`hl.plugin.hyprbar.minimize()` / `.restore()`); a client's own minimize
  request (a CSD button, X11 `IconicState`) is honored too.
- **Tray** — in-compositor SNI host with a native dbusmenu renderer. Menus
  wear the glass·ink material: a frosted, card-radius panel with a `col_frame`
  ring, accent-dim hover pills inset 4px, hairline separators — the same
  language as the notification cards. On a narrow output, only cells that fit
  the tray slot are drawn; an icon never overlaps the tasklist.
- **Bell** — the notification bell + unread badge, riding the tray's bus
  link to hyprnotify (`org.hitori.hyprnotify`). The badge counts the shade
  (popped + waiting) and hides at zero; a left click toggles the
  notification center. Hovering the bell has no notification-center action.
  DND has no bar presence — that lives in the center's ⊖ only.
- **Battery** — Google Pixel's expressive battery from SystemUI build
  `CP2A.260705.006`, transcribed from the factory image's Compose
  implementation with every frame, digit, cap, bolt, shield, plus, and
  question path embedded verbatim. The renderer also follows Pixel's child
  rounding, cap spacing, attribution overlap, level clipping, and state order:
  unknown question > Battery Saver plus > defender shield > charging bolt > D
  cap. Fill is white idle/unknown, yellow whenever Battery Saver is active,
  green charging or defending, and red at 20% or below only when no
  attribution is active. Linux Battery Saver follows the explicit
  `net.hadess.PowerProfiles` `power-saver` profile; ACPI `platform_profile`
  `low-power` is only a hardware tuning profile and never adds the plus.
  Defender maps a plugged `Not charging` pack held below a configured
  `charge_control_end_threshold`. The icon is sized 13:14 against the bar font
  and hidden only when no system battery exists.
  Alerts ride along on Android's same lines: AC plug/unplug, low at 20%,
  critical (sticky) at 5% — sent through the notification daemon off the
  same udev uevents as the gauge.
- **Clock** — `%a %b %d, %H:%M`.
- **Layoutbox** — the active workspace's layout, rightmost. Click/wheel
  cycles like awesome (`Super+Space` too); one layout until more land.
- **Menubar** (`Mod+P`, `hl.plugin.hyprbar.menubar()`) — awesome's launcher
  in its own strip below the bar: categories + `.desktop` apps filtered as
  you type, most-launched first, shell completion, history, readline
  editing. Draws above fullscreen, like awesome's ontop wibox. The raw command
  entry uses the theme's run icon; unresolved entries keep an empty reserved
  icon cell instead of substituting a character or shifting their text.

Details and the full menubar key reference: [docs/hyprbar.md](../docs/hyprbar.md).

## Config

Colors and font come from `theme.lua` via `hl.config { plugin = { hyprbar =
… } }`; the C++ defaults mirror the theme.

| key | what | default |
|---|---|---|
| `plugin:hyprbar:height` | bar height in logical px (reserve it: monitor `reserved top`) | 26 |
| `plugin:hyprbar:font_size` | text size in logical px | 12 |
| `plugin:hyprbar:tray_spacing` | px between tray icons | 10 |
| `plugin:hyprbar:font` | font family | Fira Code |
| `plugin:hyprbar:terminal` | terminal for `Terminal=true` menubar entries | alacritty |
| `plugin:hyprbar:col_bg` | bar background | `131313` |
| `plugin:hyprbar:col_fg` | normal text | `aaaaaa` |
| `plugin:hyprbar:col_focus` | selected menubar entry text | `32d6ff` |
| `plugin:hyprbar:col_active` | active tag / focused task text | `00ccff` |
| `plugin:hyprbar:col_active_bg` | active tag background | `1e2320` |
| `plugin:hyprbar:col_empty` | disabled/placeholder text | `565e6b` |
| `plugin:hyprbar:col_urgent` | urgent text | `c83f11` |
| `plugin:hyprbar:col_urgent_bg` | urgent background | `3f3f3f` |
| `plugin:hyprbar:col_square_sel` | taglist square: tag holds the focused window | `f0dfaf` |
| `plugin:hyprbar:col_square_unsel` | taglist square: occupied tag | `dcdccc` |
| `plugin:hyprbar:col_frame` | menu panel frame | `3f3f3f` |
| `plugin:hyprbar:col_charging` | battery fill charging/defending (Android's charging green) | `18cc47` |
| `plugin:hyprbar:col_low` | battery fill ≤ 20% (Android's error red) | `ff0e01` |
| `plugin:hyprbar:col_powersave` | battery fill in power save (Android's warning yellow) | `ffc917` |
