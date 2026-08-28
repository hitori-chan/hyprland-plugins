# hyprbar

A compositor-drawn top bar for every monitor:

```text
[workspaces] [tasks...........................] [tray] [bell] [battery] [clock] [layout]
```

- Workspaces are monitor-local. Click or wheel to switch; `Mod+click` moves the
  focused window without following it.
- Tasks stay in arrival order. Click to focus or minimize, right-click for the
  client list, and wheel to cycle non-minimized windows.
- The tray hosts StatusNotifier items and dbusmenu. Passive items hide and
  narrow outputs omit cells that do not fit.
- The bell shows `hyprnotify`'s resident count and toggles its center.
- The battery uses Pixel SystemUI geometry and state precedence. Only the
  PowerProfiles `power-saver` profile selects the yellow plus; charging and
  defender use green, and an unattributed level at or below 20% uses red.
- `hl.plugin.hyprbar.menubar()` opens the bounded desktop-entry and command
  launcher. The query editor supports history, completion, readline-style
  editing, and asynchronous paste with a 4 KiB UTF-8 limit.

The strip stays above maximized windows, hides below fullscreen clients, and
temporarily composites above fullscreen while the launcher is open. Detailed
input, tray, task, and launcher contracts are in
[`docs/hyprbar.md`](../docs/hyprbar.md).

## Config

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprbar:height` | bar height and required top reservation | 26 |
| `plugin:hyprbar:font_size` | text size | 12 |
| `plugin:hyprbar:tray_spacing` | tray icon gap | 10 |
| `plugin:hyprbar:font` | font family | `Roboto` |
| `plugin:hyprbar:terminal` | terminal for `Terminal=true` entries | `alacritty` |
| `plugin:hyprbar:col_bg` | panel | `ff132732` |
| `plugin:hyprbar:col_fg` | text | `ffeef3f5` |
| `plugin:hyprbar:col_muted` | compatibility fallback | `ffd1dde1` |
| `plugin:hyprbar:col_focus` | launcher selection | `ff9acbff` |
| `plugin:hyprbar:col_active` | active workspace/task | `ff9acbff` |
| `plugin:hyprbar:col_active_bg` | active/hover state | `339acbff` |
| `plugin:hyprbar:col_on_active` | content on primary | `ff102333` |
| `plugin:hyprbar:col_empty` | disabled content | `61d1dde1` |
| `plugin:hyprbar:col_urgent` | urgent content | `ffffb4ab` |
| `plugin:hyprbar:col_urgent_bg` | urgent container | `ff93000a` |
| `plugin:hyprbar:col_square_sel` | focused-window marker | `ff9acbff` |
| `plugin:hyprbar:col_square_unsel` | occupied marker | `ffd1dde1` |
| `plugin:hyprbar:col_frame` | menu outline | `33e0f0f8` |
| `plugin:hyprbar:col_charging` | charging/defender battery | `18cc47` |
| `plugin:hyprbar:col_low` | low battery | `ff0e01` |
| `plugin:hyprbar:col_powersave` | Battery Saver | `ffc917` |
