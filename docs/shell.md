# Shell

The `shell` module: the bar on every monitor and the launcher.

## The bar

A 28 px strip of glass at the top of every monitor, showing that
monitor's tags and tasks. From left to right: the tags, the tasks
(splitting the middle, 14 px off either side), then the tray, the bell,
the control center's switches, the battery and the clock, 20 px apart.

- The slot at each end owns its screen edge: the top-left pixel is the
  first tag, the top-right the clock.
- Every widget owns its cell, and the right-hand cells meet across their
  gaps, so a click on the strip never lands between two targets. A
  click on a widget with nothing to do (the battery, the clock) does
  nothing; it never reaches a neighbour.
- The glass frosts whatever lies under it, as the cards and menus do: the
  wallpaper, a window's shadow, a window floated up under the strip. It
  holds still while a window right beneath it repaints.
- Geometry is monitor-local: fixed widget slots and tray clipping keep
  the task area usable on a narrow output.

### Workspaces

Nine kanji tags (一 to 九) in 26 x 22 cells. The viewed tag is accent
text, an occupied one the bar's text, an empty one dim, an urgent one
red. There is no ground and no occupancy mark: the text's strength
says it. Hover lifts a tag's text a step.

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

StatusNotifierItem icons at 16 px in their own colours, `tray_spacing`
apart, each rastered at the monitor's pixel size (a theme's 16 px
drawing where it has one, an SVG at that size, a pixmap box-filtered
down). An item that needs attention shows its attention icon.

- **Left click** activates the item, then focuses the app's own topmost
  window. The plugin performs the raise itself because no backend can
  be trusted to: an X11 app's raise arrives as an unauthenticated
  urgency ping, and a Wayland app may never spend its activation token.
  (In awesome the app raises itself after the click; here that is
  unconditional.)
- **Middle click** is the item's `SecondaryActivate`.
- **Right click** (and left click for an `ItemIsMenu` item) opens the
  item's dbusmenu. An app that exports its menu late is re-read; an item
  with no menu gets `ContextMenu` instead, for that click only.

Tray menus (the client list's and the bell's too) are glass with
28 px rows. The row under the pointer, and the row an open submenu hangs
off, sit on a white pill (16 %, 4 px off the panel's sides) in their own
ink; the client list's focused window stays accent text. Check and radio
items show a check or a dot; a submenu's chevron has a column of its
own. They follow live updates (the hovered row stays hovered by its id),
cascade and scroll. A level with no rows says "Empty", or "Unavailable"
when the app failed to answer. Esc closes an open menu; a menu on a
monitor that goes away closes. Keyboard navigation, tooltips and
overlay icons are not implemented.

### Bell

A bell, without a count:

- a blue dot on its shoulder while the center holds a card it has not
  shown since the card arrived or changed; opening the center marks what
  it lists as seen;
- the moon under Do Not Disturb, dotted while cards wait in its queue;
- on a white pill (24 x 22, its glyph white) while the center or its
  options menu is open.

**Left click** toggles the [center](notify.md#the-center) on the clicked
bar's monitor (closing the control center). **Right click** opens the
notification options, one flat menu:

- **Quiet Automatically**, a header over its two switches: When an App
  Is Fullscreen · While Sharing the Screen. Both are off until checked;
  see [quiet automatically](notify.md#quiet-automatically).
- **Silenced Apps**, a header over a row for every app whose banners or
  sounds the card menu turned off and every muted chat: the app's icon,
  its name and, on the right, what is off ("Banners off", "Muted until
  08:00"). A click turns that one back on; while the pointer rests on a
  row its right side says so ("Turn On", "Unmute"). "None" when nothing
  is silenced.

Do Not Disturb is not here: its one home is the control center (the bell
only echoes it as the moon). Every option lives in one menu or panel.

### Control center

Two switches, the lower one on. A **left click**
toggles the [control center](notify.md#the-control-center) on the
clicked bar's monitor (closing the center); it sits on a white pill,
as the bell does, while the panel is open.

### Battery

Pixel's battery, transcribed from SystemUI: a 24 x 13 body filled to the
level, the number inside it. White normally, red at 20 % or below, green
charging (a bolt) or held at the charge limit (a shield), yellow in
power save (a plus); an unknown level draws a question mark.

It reads the udev battery packs: a dual-pack laptop reads as one, and a
USB-C port's or a headset's battery is ignored. Plugging and unplugging
post a capsule; 20 % and 5 % post the low and critical cards.

Resting the pointer on it for half a second names its state in a dark
note under the bar, its right edge 8 px past the pill's; leaving, the
lock, a real fullscreen window or an open menu takes it away, and a
click still does nothing:

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
`current_now` over the charge), smoothed over the samples the gauge
already takes (its udev events and the minute tick) and started over
when the power state flips; charging runs to the charge limit when one
holds. Until a draw is known the time is left out.

### Clock

"Mon Oct 26 01:12" in the session's `LC_TIME`. It ticks on the wall
clock's minute, re-reads the timezone each tick, and catches up at once
after a resume or a clock change.

### Text and icons

- Labels are one line: a title's control characters are replaced
  before it is drawn.
- Task, tray, menu and launcher icons decode under a time budget, so a
  menu full of new icons fills in over a few frames instead of stalling
  one. A config reload keeps resolved icons unless the GTK icon theme
  changed.

## The launcher

awesome's Mod+P prompt (`hl.plugin.awesome.menubar()`), dmenu-style: a
strip of the bar's glass right under it with "Run:" (or the opened
category), the query and its caret, then the matches 20 px apart, each a
16 px icon and its name, the selection in accent text.

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

- **Entries** come from the plugin's one desktop index (shared with the
  bar's and the cards' icons). It is scanned at start; an applications
  directory that changed since is rescanned on the next open, the old
  list shown until the new one lands whole.
- **Running**: the typed line runs through `/bin/sh -c`. A completed name
  is inserted as one escaped word (a file named `x$(cmd)` stays a file
  name), and a name with a control or bidi character is never offered.
  `Exec=` keeps its quoting and field codes; `Terminal=true` entries run
  in `terminal`.
- **Completion**: the command word completes from `$PATH`, or from its
  own directory when typed as a path (`./run`, `~/bin/x`). Tab applies
  the first match of the whole sorted scan (capped at 512 names, or what
  two seconds gathered from a stalled mount).
- **Activation**: every launch carries a fresh activation token
  (`XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID`, or a D-Bus `Activate`'s
  platform data), so an app that is already running can raise itself.
- **Keys**: held keys repeat at the keyboard's own delay and rate. A
  layer that takes the keyboard (a power menu opened over the prompt)
  closes the prompt instead of stealing its keys; so does its monitor
  going away.
- **Limits**: typed, pasted and completed input share a 4 KiB UTF-8
  limit; a paste drops control, bidi and line-separator characters and
  malformed bytes, and gives up after 1.5 s. History and the ranking live in the
  [state file](reference.md#state).
