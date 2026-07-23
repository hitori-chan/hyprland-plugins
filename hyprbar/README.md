# hyprbar

The shell bar, drawn by the compositor — two modes on one machinery
(`plugin:hyprbar:mode`), living in a 30px band (`reserved = { top = 30 }`,
matching `plugin:hyprbar:height`):

- **islands** (the default): a transparent band holding 26px frosted-glass
  pills, superellipse corners.
- **strip**: ONE full-bleed frosted band — `col_bg`'s RGB at `bar_alpha`,
  flat (no hairlines, no gradients), with baked-in grain and a soft
  under-shadow. Every cell runs the full band height, so y=0 and both
  screen corners are live click targets (the top-left throw lands tag 一),
  and the menubar docks instead of floating.

Both are identical maximized or not, and hide under real fullscreen (except
the open menubar, which floats above even that). The band owns its pointer —
hovering it never leaks cursor shape or focus to a window underneath, and a
click anywhere on it is the shell's, never a window's. The skin is glass·ink
(`common/theme.hpp`) with live blur (the strip is a single blur region).

```
islands:  ( 一..九 )  (chip) (chip)         ( en  tray  bell  batt  14:32 )
strip:    |一..九| chip | chip |            en  tray  bell  batt  Wed Jul 23, 14:32|
```

- **Taglist** (left) — the nine kanji. Active = accent on an accent-dim
  fill (islands: a rounded pill; strip: a full-height wash + a 2px accent
  baseline); urgent = the kanji in the urgent color and nothing else
  (viewing the tag clears it, Android-style); occupied = ink; empty =
  muted. Click views, `Mod+click` sends the focused window, wheel cycles.
  Strip cells sit flush to the corner, contiguous.
- **Task chips** (the middle) — one chip per window of the active
  workspace, arrival order: 15px themed app icon (75% → 100% on focus,
  resolver shared via `common/icons.hpp`) + `⌃`/`+`/`✈` markers + title,
  max-w 220. Islands: 24px pills, gap 6; strip: full-height square
  segments, 1px apart. The focused chip fills accent-dim; urgent tints;
  minimized keeps its chip, muted. Left = focus (focused again =
  minimize), middle = close, wheel cycles focus. `Mod+N` / `Mod+Ctrl+N`
  minimize/restore-last; a client's own minimize request is honored.
- **Status island** (right) — layout chip · tray → bell → battery →
  time, gap 7, no separators, glyphs full ink. (No wifi wedge: nm-applet's
  SNI icon in the tray already carries the strength.)
  - **layout chip** — the active keyboard layout, two letters (`en`, `vi`).
  - **tray** — SNI host, 24×24 cells with 15px icons; left `Activate`,
    middle `SecondaryActivate`, right the dbusmenu as a glass panel
    (radius 12, h26 rows, accent-dim hover; esc or an outside click
    closes).
  - **bell** — Material's filled bell + the badge (live + kept, hides at
    zero; 15px accent circle). A click toggles hyprnotify's center over
    the bus (`org.hitori.hyprnotify`). DND has NO bar presence — that
    state lives in the center's ⊖ only.
  - **battery** — Android's expressive pill, unchanged: digits inside,
    the attribution ladder (power-save plus / defender shield / charging
    bolt / D cap), fill ink · accent charging · urgent ≤20% · gold in
    power save. The plug/low/critical alerts ride the same udev uevents.
  - **time** — the bold clock; `plugin:hyprbar:clock_format` (strftime,
    ticks per minute — no `%S`) formats it. Default `%H:%M`; awesome's
    stock textclock is `%a %b %d, %H:%M`.
- **Menubar** (`Mod+P`) — the launcher below the band. Islands: a floating
  glass pill (inset 6, offset 34), `run:` prompt over a right hairline,
  pill chips (h24, selected = accent-dim fill). Strip: DOCKED — a second
  full-width band row sliding straight out of the strip, one tone up
  (`col_bar_menubar`), no gap/radius/float/hairline, full-height square
  chips (selected = solid accent, ink inverted). Filtering, completion,
  history and readline editing identical in both; the keyboard hint rides
  the right edge. Draws above fullscreen.

Details and the full menubar key reference: [docs/hyprbar.md](../docs/hyprbar.md).

## Config

Colors and font come from `theme.lua` via `hl.config { plugin = { hyprbar =
… } }`; the C++ defaults ARE the glass·ink tokens.

| key | what | default |
|---|---|---|
| `plugin:hyprbar:mode` | `islands` \| `strip` | islands |
| `plugin:hyprbar:height` | band height in logical px (islands are height−4; reserve it) | 30 |
| `plugin:hyprbar:font_size` | text size in logical px | 12 |
| `plugin:hyprbar:tray_spacing` | px between tray icons | 3 |
| `plugin:hyprbar:rounding_power` | corner superellipse exponent | 3.0 |
| `plugin:hyprbar:bar_alpha` | strip: the band's glass alpha over `col_bg`'s RGB | 0.62 |
| `plugin:hyprbar:font` | font family | IBM Plex Sans |
| `plugin:hyprbar:clock_format` | strftime clock text (ticks per minute) | `%H:%M` |
| `plugin:hyprbar:terminal` | terminal for `Terminal=true` menubar entries | foot |
| `plugin:hyprbar:col_bg` | island glass / the strip's RGB (alpha is the islands' glass) | `9e0f1218` |
| `plugin:hyprbar:col_bar_menubar` | strip: the docked menubar row (one tone up) | `a8181d26` |
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
