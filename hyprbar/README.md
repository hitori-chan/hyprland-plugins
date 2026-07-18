# hyprbar

The AwesomeWM wibar, drawn by the compositor. Renders in each monitor's
reserved top strip (`reserved = { top = 26 }`, matching
`plugin:hyprbar:height`); hides under real fullscreen — except while the
menubar is open, which floats above even that; maximized windows keep it
visible. The strip owns its pointer — hovering it never leaks cursor shape
or focus to a window underneath.

```
[taglist 一..九] [tasklist of the active workspace ...] [tray] [battery] [clock] [layoutbox]
```

- **Taglist** — kanji buttons, awesome's state matrix; occupancy as the
  corner square. Click views, `Mod+click` sends the focused window, wheel
  cycles.
- **Tasklist** — the active workspace's windows in arrival order: app icon,
  `⌃` pinned / `+` maximized / `✈` floating markers. Click focuses + raises, right-click
  opens the all-clients menu, wheel walks focus.
- **Tray** — in-compositor SNI host with a native dbusmenu renderer. Menus
  wear the overlay language: 1px-rounded panel with a `col_frame` ring,
  hover rows inset 4px with softened corners.
- **Battery** — Android's expressive battery (the Pixel pill of Android 16
  QPR2/17), a 1:1 transcription of SystemUI's Compose implementation with
  its vector assets embedded verbatim, bespoke digit glyphs included:
  borderless pill on a translucent track, left-anchored fill, D cap right
  — replaced by Android's attribution ladder: the power-save plus (ACPI
  platform profile `low-power`), the defender shield (plugged but held at
  `charge_control_end_threshold`), or the charging bolt — digits punched
  in black. Fill white idle / yellow in power save / green charging or
  defending / red ≤ 20%; sized 13:14 against the bar font, the proportion
  Android gives it against status bar text; hidden on desktops.
  Alerts ride along on Android's same lines: AC plug/unplug, low at 20%,
  critical (sticky) at 5% — sent through the notification daemon off the
  same udev uevents as the gauge.
- **Clock** — `%a %b %d, %H:%M`.
- **Layoutbox** — the active workspace's layout, rightmost. Click/wheel
  cycles like awesome (`Super+Space` too); one layout until more land.
- **Menubar** (`Mod+P`, `hl.plugin.hyprbar.menubar()`) — awesome's launcher
  in its own strip below the bar: categories + `.desktop` apps filtered as
  you type, most-launched first, shell completion, history, readline
  editing. Draws above fullscreen, like awesome's ontop wibox. Iconless
  entries keep their cell with a letter fallback instead of collapsing.

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
| `plugin:hyprbar:col_muted` | tray letter fallback | `8a97a8` |
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
