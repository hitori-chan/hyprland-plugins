# hyprbar

The compact-islands shell bar, drawn by the compositor. ONE state: a 30px
transparent band (`reserved = { top = 30 }`, matching
`plugin:hyprbar:height`) holding 26px frosted-glass pills — identical
maximized or not; hides under real fullscreen (except the open menubar,
which floats above even that). The band owns its pointer — hovering it
never leaks cursor shape or focus to a window underneath. The skin is
glass·ink (`common/theme.hpp`): live-blur islands, IBM Plex Sans,
superellipse corners.

```
( 一..九 )  (chip) (chip) (chip)            ( en  tray  bell  wifi  batt  14:32 )
```

- **Taglist** (left island) — the nine kanji. Active = accent on an
  accent-dim fill; urgent = the kanji in the urgent color and nothing else
  (viewing the tag clears it, Android-style); occupied = ink; empty =
  muted. Click views, `Mod+click` sends the focused window, wheel cycles.
- **Task chips** (the middle) — one pill per window of the active
  workspace, arrival order: 15px themed app icon (75% → 100% on focus,
  resolver shared via `common/icons.hpp`) + `⌃`/`+`/`✈` markers + title,
  max-w 220. The focused chip fills accent-dim; urgent tints; minimized
  keeps its chip, muted. Left = focus (focused again = minimize), middle =
  close, wheel cycles focus. `Mod+N` / `Mod+Ctrl+N` minimize/restore-last;
  a client's own minimize request is honored.
- **Status island** (right), the decided order — layout chip · tray →
  bell → wifi → battery → time, gap 7, no separators, glyphs full ink:
  - **layout chip** — the active keyboard layout, two letters (`en`, `vi`).
  - **tray** — SNI host, 24×24 cells with 15px icons; left `Activate`,
    middle `SecondaryActivate`, right the dbusmenu as a glass panel
    (radius 12, h26 rows, accent-dim hover; esc or an outside click
    closes).
  - **bell** — Material's filled bell + the badge (live + kept, hides at
    zero; 15px accent circle). A click toggles hyprnotify's center over
    the bus (`org.hitori.hyprnotify`). DND has NO bar presence — that
    state lives in the center's ⊖ only.
  - **wifi** — the Android segmented wedge (dot + two stroked arcs);
    partial strength dims segments to 25%, off adds the slash. An
    indicator only; hidden without wireless hardware.
  - **battery** — Android's expressive pill, unchanged: digits inside,
    the attribution ladder (power-save plus / defender shield / charging
    bolt / D cap), fill ink · accent charging · urgent ≤20% · gold in
    power save. The plug/low/critical alerts ride the same udev uevents.
  - **time** — the bold `HH:MM`, nothing else.
- **Menubar** (`Mod+P`) — the launcher in a floating glass pill below the
  band (inset 6, offset 34): `run:` prompt, category/app chips (h24, 15px
  icons, selected = accent fill), most-launched-first filtering, shell
  completion, history, readline editing; the keyboard hint rides the right
  edge. Draws above fullscreen.

Details and the full menubar key reference: [docs/hyprbar.md](../docs/hyprbar.md).

## Config

Colors and font come from `theme.lua` via `hl.config { plugin = { hyprbar =
… } }`; the C++ defaults ARE the glass·ink tokens.

| key | what | default |
|---|---|---|
| `plugin:hyprbar:height` | band height in logical px (islands are height−4; reserve it) | 30 |
| `plugin:hyprbar:font_size` | text size in logical px | 12 |
| `plugin:hyprbar:tray_spacing` | px between tray icons | 3 |
| `plugin:hyprbar:rounding_power` | corner superellipse exponent | 3.0 |
| `plugin:hyprbar:font` | font family | IBM Plex Sans |
| `plugin:hyprbar:terminal` | terminal for `Terminal=true` menubar entries | foot |
| `plugin:hyprbar:col_bg` | island glass (alpha is the glass) | `9e0f1218` |
| `plugin:hyprbar:col_fg` | full-ink text and status glyphs | `e4e8ee` |
| `plugin:hyprbar:col_muted` | secondary text, letter fallbacks | `98a2ac` |
| `plugin:hyprbar:col_focus` | selected menubar entry text | `32d6ff` |
| `plugin:hyprbar:col_active` | active tag / focused task text | `32d6ff` |
| `plugin:hyprbar:col_active_bg` | active/selected fills (accent-dim) | `2932d6ff` |
| `plugin:hyprbar:col_empty` | empty tags, disabled text | `8098a2ac` |
| `plugin:hyprbar:col_urgent` | urgent text | `ff8a5c` |
| `plugin:hyprbar:col_urgent_bg` | urgent chip fill | `29ff8a5c` |
| `plugin:hyprbar:col_frame` | hairlines | `17dcebff` |
| `plugin:hyprbar:col_charging` | battery fill charging/defending | `32d6ff` |
| `plugin:hyprbar:col_low` | battery fill ≤ 20% | `ff8a5c` |
| `plugin:hyprbar:col_powersave` | battery fill in power save | `ffc917` |
