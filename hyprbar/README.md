# hyprbar

An AwesomeWM-style wibar drawn by the compositor in each monitor's reserved top
strip. It stays visible for maximized windows, hides under fullscreen, and
temporarily draws above composited fullscreen while the menubar is open.

```text
[tags] [tasks................................] [tray] [bell] [battery] [clock] [layout]
```

- **Tags:** monitor-local kanji workspace buttons with focused, occupied, and
  urgent states. Click or wheel changes that monitor's workspace;
  `Mod+click` moves the focused window without following it.
- **Tasks:** active-workspace windows in arrival order with app icon and state
  markers. Click the focused task to minimize it; click another to restore and
  focus it. Right-click opens the client list; wheel walks non-minimized tasks.
- **Tray:** native StatusNotifier host and dbusmenu renderer. Passive items hide,
  attention icons update, and narrow outputs drop cells that do not fit.
- **Bell:** shows the `hyprnotify` center count and toggles the notification
  center on left click. DND remains in the center.
- **Battery:** Pixel SystemUI `CP2A.260705.006` geometry, glyphs, rounding,
  clipping, and state order. The question, Battery Saver plus, defender shield,
  charging bolt, and cap use Pixel's exact paths. Only
  `net.hadess.PowerProfiles` `power-saver` selects the yellow plus; ACPI
  `platform_profile=low-power` does not. Charging/defender is green, an
  unattributed level at or below 20% is red, and alerts fire at 20% and 5%.
  Battery alert popups use the native battery identity icon.
- **Clock/layout:** fixed clock text and per-workspace layout state. Click or
  wheel the layout icon to cycle the registry.
- **Menubar:** `Mod+P` or `hl.plugin.hyprbar.menubar()` opens the application and
  command launcher below the bar. It supports desktop entries, categories,
  history, completion, and readline-style editing.

Tasklist lifecycle, tray/bus behavior, native input precedence, and the complete
menubar key map are in [docs/hyprbar.md](../docs/hyprbar.md).

## Config

Colors and fonts normally come from `theme.lua`; these are the C++ defaults.

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprbar:height` | bar height in logical px; reserve the same monitor top area | 26 |
| `plugin:hyprbar:font_size` | text size | 12 |
| `plugin:hyprbar:tray_spacing` | spacing between tray icons | 10 |
| `plugin:hyprbar:font` | font family | Fira Code |
| `plugin:hyprbar:terminal` | terminal for launcher entries | alacritty |
| `plugin:hyprbar:col_bg` | bar background | `131313` |
| `plugin:hyprbar:col_fg` | normal text | `aaaaaa` |
| `plugin:hyprbar:col_muted` | legacy compatibility fallback | `8a97a8` |
| `plugin:hyprbar:col_focus` | selected launcher text | `32d6ff` |
| `plugin:hyprbar:col_active` | active tag/focused task | `00ccff` |
| `plugin:hyprbar:col_active_bg` | active tag background | `1e2320` |
| `plugin:hyprbar:col_empty` | disabled/placeholder text | `565e6b` |
| `plugin:hyprbar:col_urgent` | urgent text | `c83f11` |
| `plugin:hyprbar:col_urgent_bg` | urgent background | `3f3f3f` |
| `plugin:hyprbar:col_square_sel` | focused-window tag marker | `f0dfaf` |
| `plugin:hyprbar:col_square_unsel` | occupied tag marker | `dcdccc` |
| `plugin:hyprbar:col_frame` | menu frame | `3f3f3f` |
| `plugin:hyprbar:col_charging` | charging/defender fill | `18cc47` |
| `plugin:hyprbar:col_low` | battery fill at or below 20% | `ff0e01` |
| `plugin:hyprbar:col_powersave` | Battery Saver fill | `ffc917` |
