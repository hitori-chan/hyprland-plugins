# Shell

The `shell` module: the bar on every monitor and the launcher.

## The bar

A 28 px strip of glass at the top of every monitor, showing that
monitor's tags and tasks. From left to right: the tags, the tasks
(splitting the middle, 14 px off either side), then the tray, the bell,
the battery and the clock, 15 px apart. The first tag stands at the
screen's left edge, the clock 8 px off the right one.

- Every widget owns its cell and the cells meet across their gaps, so a
  click on the strip never lands between two targets: the top-left pixel
  is the first tag, the top-right the clock. A click on the battery or
  the clock does nothing.
- Geometry is monitor-local. On a narrow output the right slot never
  crosses the tags: its leftmost widgets yield first, and the tasks take
  what is left.
- The bar reserves its own band on every monitor (the fork's plugin
  reserved type): maximized, snapped and placed windows start below it,
  with no `reserved_area` in the monitor rule. A rule's own reserved top
  counts toward the band.

### A display notch

On a laptop panel with a notch (Apple silicon booted with
`appledrm.show_notch=1`), the bar fills the band beside the notch: it is
as tall as the band (37 px on the 14" and 16" M-series panels at scale
2) where that is taller than `height`, and splits around the notch. The
tags and the tasks end before it, the tray, bell, battery and clock
start after it, and no widget is drawn or clicked under it (the glass
runs on underneath).

- The band's height comes from the firmware (the device tree's
  `apple,notch-height` on the enabled display node); the notch shows
  when the mode is the 16:10 panel plus exactly that band, on the
  built-in panel (`eDP`), unrotated.
- Nothing reports the notch's width: it is estimated from the height,
  and `notch_width` sets it in logical px.
- A fullscreen window still covers the whole panel, the notch's corners
  included (its direct scanout needs the full mode).

### Workspaces

Nine kanji tags (一 to 九) in 26 px cells. The viewed tag stands on
awesome's `bg_focus`, a flat square cell the bar's full height (white
8 %), its text accent at semibold. An occupied tag is the bar's text, an
empty one dim, an urgent one red; there is no other mark. Hover lifts a
tag's text a step.

| Input | Action (awesome's taglist buttons) |
|---|---|
| click | view the tag, creating it on this monitor (a tag another monitor owns is left alone) |
| `Mod`+click | send the focused window there without following it |
| wheel | step through this monitor's tags, wrapping |

### Tasks

Each task is a 16 px icon and the title, in arrival order. ⌃ marks a
pinned window and + a maximized one. The focused task is accent text, an
urgent one red, a minimized one dim with its icon at half strength.

| Input | Action (awesome's tasklist buttons) |
|---|---|
| click on the focused task | minimize it |
| click on a minimized task | restore it |
| click on any other | raise and focus it |
| wheel | cycle the focus through this workspace's tasks (minimized ones are listed, not walked) |
| right click | awesome's client list: every window on any tag, its tag in a right-hand column; a click goes to it, restoring a minimized one |

X11 override-redirect surfaces (menus, tooltips, dropdowns) are not
clients, as in awesome: never a task, a tag occupant, a focus stop, a
snap edge or a placement blocker.

### Tray

StatusNotifierItem icons at 16 px in their own colours (a symbolic icon
in `col_fg`), `tray_spacing` apart, rastered once for the densest
monitor's scale. An item that needs attention shows its attention icon.

- **Left click** activates the item, then focuses the app's own topmost
  window. The plugin raises it itself: an X11 app's raise arrives as an
  unauthenticated urgency ping, and a Wayland app may never spend its
  activation token.
- **Middle click** is the item's `SecondaryActivate`.
- **Right click** (and left click for an `ItemIsMenu` item) opens the
  item's dbusmenu. An app that exports its menu late is re-read; an item
  with no menu gets `ContextMenu` instead, for that click only.

Tray menus (the client list's and the bell's too) are glass with 28 px
rows. The row under the pointer, and the row an open submenu hangs off,
sit on the white pill; the client list's focused window stays accent
text. Check and radio items show a check or a dot; a submenu's chevron
has a column of its own. Menus follow live updates (the hovered row
stays hovered by its id), cascade and scroll. A level with no rows says
"Empty", or "Unavailable" when the app failed to answer. Esc closes an
open menu; a menu on a monitor that goes away closes. Icons decode
under a time budget, so a menu full of new icons fills in over a few
frames. Keyboard navigation, tooltips and overlay icons are not
implemented.

### Bell

A bell, without a count:

- a blue dot on its shoulder while the center holds a card it has not
  shown since the card arrived or changed; opening the center marks what
  it lists as seen;
- the moon under Do Not Disturb, dotted while cards wait in its queue;
- on a white pill (24 x 22, its glyph white) while the center or its
  options menu is open.

**Left click** toggles the [center](notify.md#the-center) on the clicked
bar's monitor. **Right click** opens the notification options, one flat
menu:

- **Quiet Automatically**, a header over its two switches: When an App
  Is Fullscreen · While Sharing the Screen (see
  [quiet automatically](notify.md#quiet-automatically)).
- **Silenced Apps**, a header over a row for every app whose banners or
  sounds the card menu turned off and every muted chat: the app's icon,
  its name and, on the right, what is off ("Banners off", "Muted until
  08:00"). A click turns that one back on; while the pointer rests on a
  row its right side says so ("Turn On", "Unmute"). "None" when nothing
  is silenced.

Do Not Disturb is set from the center's moon (or a bind's `suspend()`);
the bell only echoes it
([one home per option](design.md#one-home-per-option)).

### Battery

A 24 x 13 battery body filled to the level, the number inside it. White
normally, red at 20 % or below, green charging (a bolt) or held at the
charge limit (a shield), yellow in power save (a plus); an unknown level
draws a question mark.

It reads the udev battery packs: a dual-pack laptop reads as one, and a
USB-C port's or a headset's battery is ignored. With no pack it is not
shown. A change to charging or to discharging posts a capsule; 20 % and
5 % post the low and critical cards.

Resting the pointer on it for half a second names its state in a dark
note under the bar; leaving, the lock, a real fullscreen window or an
open menu takes it away:

| State | The note |
|---|---|
| on battery | 76% · 3:12 left |
| power save | 34% · Power Saver · 2:05 left |
| charging | 63% · Charging · full at 22:40 |
| charged | 100% · Charged |
| held at the charge limit | Held at 80% to spare the battery |
| on the cable, not charging | 92% · Not charging |
| unknown | Battery status unknown |

The time comes from the packs' draw (`power_now` over the energy, else
`current_now` over the charge), smoothed over the gauge's samples (its
udev events and the minute tick) and started over when the power state
flips; charging runs to the charge limit when one holds. Until a draw is
known the time is left out.

### Clock

"Mon Oct 26 01:12" in the session's `LC_TIME`. It ticks on the wall
clock's minute, re-reads the timezone each tick, and catches up at once
after a resume or a clock change.

## The launcher

awesome's Mod+P prompt (`hl.plugin.awesome.menubar()`), dmenu-style: a
strip of the bar's glass right under it with "Run:" (or the opened
category), the query and its accent caret, then the matches 20 px apart,
each a 16 px icon and its name. The selection is the accent name on the
viewed tag's flat cell. While the strip is open, the cards sit under it.

| Keys | Action |
|---|---|
| `Left`/`Right`, `C-j`/`C-k`, `Home`/`End` | select |
| `Enter` | run the selected entry |
| `C-Return`, `C-M-Return` | run the query as typed, directly or in a terminal |
| `Tab`, `Shift-Tab` | complete the command or path |
| `Up`/`Down`, `C-p`/`C-n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace`, `Delete` | edit |
| `C-v` | paste |
| `Escape`, or `BackSpace` on an empty query | leave an opened category |
| `Escape` with no category, or a click | close |

- **Entries** come from the plugin's one desktop index. An applications
  directory that changed since the last scan is rescanned on the next
  open, the old list shown until the new one lands whole.
- **Running**: the typed line runs through `/bin/sh -c`. A completed name
  is inserted as one escaped word (a file named `x$(cmd)` stays a file
  name), and a name with a control or bidi character is never offered.
  `Exec=` keeps its quoting and field codes; `Terminal=true` entries run
  in `terminal`. Every launch carries a fresh activation token
  (`XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID`, or a D-Bus `Activate`'s
  platform data), so an app already running can raise itself.
- **Completion**: the command word completes from `$PATH`, or from its
  own directory when typed as a path (`./run`, `~/bin/x`). Tab applies
  the first match of the whole sorted scan (capped at 512 names, or what
  two seconds gathered from a stalled mount).
- **Keys**: held keys repeat at the keyboard's own delay and rate. A
  layer that takes the keyboard (a power menu opened over the prompt)
  closes the prompt instead of stealing its keys; so does its monitor
  going away.
- **Limits**: typed, pasted and completed input share a 4 KiB UTF-8
  limit. A paste turns newlines and tabs into spaces, drops other
  control, bidi and line-separator characters and malformed bytes, and
  gives up after 1.5 s. History and the ranking live in the
  [state file](reference.md#state).
