# hyprbar details

## The band

One state (decided with the redesign): a 30px transparent band, 26px glass
islands inside — the taglist pill left, the task chips filling the middle,
the status island right. Nothing relayouts when a window maximizes; real
fullscreen hides the band (the open menubar still floats above it). The
islands are glass·ink: `col_bg`'s alpha is the frost, blur rides the
compositor's `decoration:blur` (the pass element declares live blur), each
island paints its own soft shadow.

## Taglist

The nine kanji in the left island. The state matrix: the viewed tag =
accent text on an accent-dim fill (radius 8); an urgent tag = the kanji in
the urgent color and NOTHING else; occupied = full ink; empty = muted.
Viewing a tag clears its urgency, Android-style (the compositor only clears
the flag on focus; the bar remembers what was seen). Click views,
`Mod+click` sends the focused window silently, wheel cycles wrapping.

## Task chips

One pill per window of the active workspace, arrival order, stable across
raises: a 15px themed app icon (75% opacity at rest, full on focus;
resolved through `common/icons.hpp`), the state markers, the title
(ellipsized, max-w 220; chips shrink together when the strip runs out).

- `⌃` — pinned (presented as awesome's ontop marker)
- `+` — maximized
- `✈` — floating, suppressed while maximized

The focused chip fills accent-dim with accent text; urgent chips tint
urgent; minimized chips stay, muted. Left = focus (the focused chip again =
minimize); middle = close; wheel walks focus, skipping minimized. The
all-clients menu left with the redesign — the chips are the list.

**Minimize** is unchanged: the plugin hides the window (a tiled window is
dropped from the layout; any fullscreen/maximize mode is held and
re-entered on restore) and tracks it in a stack. `Mod+N` minimizes,
`Mod+Ctrl+N` restores the most recent (`hl.plugin.hyprbar.minimize()` /
`.restore()`); a client's own minimize request (CSD button, X11
`IconicState`) is honored.

## The status island

Left → right: layout chip · tray → bell → battery → time. Gap 7, no
separators; every glyph full ink — state alone recolors. There is no wifi
wedge: nm-applet's SNI icon in the tray already carries the strength
(user call 2026-07-23), so a second glyph would say it twice.

- **Layout chip** — the active keyboard layout's two letters (`en`, `vi`),
  updated by the `keyboard.layout` event. An indicator (layouts switch by
  keybind, as always).
- **Tray** — StatusNotifierWatcher/Host, 24×24 cells with 15px icons.
  Left Activate (menu-only items open their menu), middle
  SecondaryActivate, right the dbusmenu; scroll forwards to the app.
  Passive hides, NeedsAttention swaps the icon set. The menu is a glass
  panel now — radius 12, h26 rows, accent-dim hover (radius 8), 1px inset
  separators — and closes on pick, outside click, or esc; submenus still
  cascade on GTK's 225ms delay, over-tall panels still scroll.
- **Bell** — Material's filled notifications shape, 16px, with the badge
  (live + kept, hides at zero; min-w 15, 9/700 on the accent). A click
  calls `Toggle` on hyprnotify's `org.hitori.hyprnotify` bus face; the
  badge rides its `State` signal. DND has no bar presence — the center's
  ⊖ owns that state.
- **Battery** — Android's expressive pill, kept exactly as shipped
  (transcribed 1:1 from SystemUI): digits inside, the attribution ladder
  (power-save plus > defender shield > charging bolt > the D cap), fill
  ink · accent charging/defending · urgent ≤20% · gold in power save.
  The AC plug/unplug, low (20%) and critical (5%, sticky) alerts ride the
  same udev uevents, sent through the notification daemon.
- **Time** — the bold `HH:MM` (Android 16's bold status clock), nothing
  else. No date, no click action.

## The menubar (`Mod+P`)

The launcher in a floating glass pill below the band (inset 6, below-bar
offset 34, full pill radius): a `run:` prompt over a right hairline, then
category/app chips (h24, pad-x 11, 15px icons, gap 5; the selected chip
fills accent-dim with accent text), the keyboard hint right-aligned.
Filtering is unchanged — name or command line, substring, prefix matches
and most-launched first, plus the trailing `Exec: <query>` entry.

| keys | action |
|---|---|
| `Left/Right`, `C-j/k` | select |
| `Home/End` | jump |
| `Enter` | run (`Terminal=true` entries open in `plugin:hyprbar:terminal`) |
| `C-Return` | run the raw query |
| `C-M-Return` | run the raw query in the terminal |
| `Tab`/`Shift-Tab` | shell completion (`$PATH` for the command word, filenames after) |
| `Up/Down`, `C-p/n` | prompt history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | readline editing |
| `Escape`, any click | close |

Iconless entries keep their cell with a letter fallback. Launch counts and
history persist in `~/.cache/hyprbar/`. This is the only launcher.

## The layout registry

The layoutbox cell left the bar with the redesign; the per-tag registry and
its chords stay (`Super+Space` / `Super+Shift+Space`,
`hl.plugin.hyprbar.layout_next()`/`layout_prev()`). One layout (`floating`)
until more are implemented — a future layout engine enforces it.

## Limitations

- The menubar's cursor is a `▏` bar, not an inverse-video block — the text
  renderer takes plain text, no pango markup.
- The tray is mouse-only beyond esc-to-close: no tooltips, overlay icons or
  menu keyboard navigation. SVG-only themed icons fall back to a letter.
- Over a direct-scanout fullscreen game the menubar cannot draw: scanout
  (`render:direct_scanout`) hands the plane to the client and bypasses
  compositing. Composited fullscreen (video, browser) is fine.
- Backlog: a manual idle-inhibit "coffee" tray toggle.
