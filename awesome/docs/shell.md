# Shell

The `shell` module: the bar on every monitor and the launcher.

## The bar

A 28 px glass strip atop every monitor, showing that monitor's tags and
tasks. Left to right: the tags, the tasks (the middle, 14 px off either
side), then the tray, bell, battery and clock, 15 px apart. The first tag
touches the left edge; the clock sits 8 px off the right.

- Cells meet across their gaps, so a click on the strip always has a
  target: the top-left pixel is the first tag, the top-right the clock.
  The battery and clock ignore clicks. Side buttons (back, forward) do
  nothing on the strip and never reach a window beneath it.
- Geometry is per monitor. On a narrow output the right slot never
  crosses the tags: its leftmost widgets yield first (the tray drops
  icons, the others show whole or not at all), and the tasks take the
  rest.
- The bar reserves its band (the fork's plugin reserved type), so
  maximized, snapped and placed windows start below it without
  `reserved_area`. A monitor rule's reserved top adds to it.

### Workspaces

Nine kanji tags (一 to 九), 26 px cells. The viewed tag is accent
semibold text on awesome's `bg_focus` (a flat white 8 % cell, full
height). Occupied tags are bar text, empty ones dim, urgent ones red.
Hover lifts the text a step.

| Input | Action (awesome's taglist buttons) |
|---|---|
| click | view the tag, creating it on this monitor (a tag another monitor owns is left alone) |
| `Mod`+click | send the focused window there without following |
| wheel | step through this monitor's tags, wrapping |

### Tasks

A 16 px icon and the title per window, in arrival order; ⌃ marks pinned,
+ maximized. Focused is accent text, urgent red, minimized dim with a
half-strength icon.

| Input | Action (awesome's tasklist buttons) |
|---|---|
| click on the focused task | minimize it |
| click on a minimized task | restore it |
| click on another | raise and focus it |
| wheel | cycle focus through this workspace's tasks (minimized ones are skipped) |
| right click | awesome's client list: every window, its tag in a right column; a click goes to it, restoring a minimized one |

X11 override-redirect surfaces (menus, tooltips) are not clients, as in
awesome: never a task, tag occupant, focus stop, snap edge or placement
blocker.

### Tray

StatusNotifierItem icons, 16 px in their own colours (symbolic ones in
`col_fg`), `tray_spacing` apart; an item needing attention shows its
attention icon.

- **Left click** activates the item, then raises and focuses the app's
  topmost window itself (an X11 raise arrives as an unauthenticated
  urgency ping; a Wayland app may never spend its token).
- **Middle click**: `SecondaryActivate`.
- **Right click** (left for an `ItemIsMenu` item) opens its dbusmenu,
  re-read if the app exports it late; an item with no menu gets
  `ContextMenu` for that click.

Menus (tray, client list, bell) are glass with 28 px rows; the hovered
row, and the row an open submenu hangs off, sit on the white pill. Check
and radio items show a check or dot; submenu chevrons get their own
column. Menus follow live updates (hover kept by id), cascade and scroll.
An empty level says "Empty", or "Unavailable" when the app did not
answer. Esc closes; so does losing the monitor. Icons decode under a
budget and fill in over a few frames. No keyboard navigation, tooltips
or overlay icons.

### Bell

No count. It shows:

- a blue dot while the center holds a card it has not shown since it
  arrived or changed (opening the center marks its list seen);
- the moon under Do Not Disturb, dotted while arrivals wait;
- a white pill while the center or its menu is open.

**Left click** toggles the [center](notify.md#the-center) on that
monitor. **Right click** opens the notification options:

- **Quiet Automatically**: When an App Is Fullscreen · While Sharing the
  Screen ([quiet automatically](notify.md#quiet-automatically)).
- **Silenced Apps**: every app whose banners or sounds the card menu
  turned off and every muted chat, with what is off ("Banners off",
  "Muted until 08:00"). A click turns it back on (the row says "Turn On"
  or "Unmute" under the pointer). "None" when empty.

Do Not Disturb is set from the center's moon or `suspend()`; the bell
only echoes it ([one home per option](design.md#one-home-per-option)).

### Battery

A 24 x 13 body filled to the level, the number inside. White normally,
red at ≤ 20 %, green charging (a bolt) or held at the charge limit (a
shield), yellow in power save (a plus); `?` when unknown. Hidden without
a battery.

The udev packs read as one (a dual-pack laptop); USB-C and headset
batteries are ignored. Packs are read at load, then by a helper on each
pack uevent and every minute: their attributes evaluate ACPI methods
(milliseconds each on a ThinkPad), so the compositor never waits on one.
A change to charging or discharging posts a capsule; 20 % and 5 % post
the low and critical cards.

After 500 ms under the pointer a dark note names its state; leaving, the
lock, a real fullscreen window or an open menu removes it:

| State | Note |
|---|---|
| on battery | 76% · 3:12 left |
| power save | 34% · Power Saver · 2:05 left |
| charging | 63% · Charging · full at 22:40 |
| charged | 100% · Charged |
| held at the charge limit | Held at 80% to spare the battery |
| plugged, not charging | 92% · Not charging |
| unknown | Battery status unknown |

The time is the packs' draw (`power_now` over energy, else
`current_now` over charge), smoothed over the samples and restarted when
the power state flips; charging runs to the charge limit when one holds.
No draw yet, no time.

### Clock

`shell:clock_format` in the session's `LC_TIME` (default "Mon Oct 26
01:12"), applied at once. It ticks on the wall clock's minute, re-reads
the timezone each tick, and catches up at once after a resume or clock
change.

## The launcher

awesome's Mod+P prompt (`hl.plugin.awesome.menubar()`), dmenu-style: a
glass strip under the bar with "Run:" (or the open category), the query
and an accent caret, then the matches 20 px apart (16 px icon, name).
The selection is the accent name on a flat cell. The cards sit below the
open strip.

| Keys | Action |
|---|---|
| `Left`/`Right`, `C-j`/`C-k`, `Home`/`End` | select |
| `Enter` | run the selection |
| `C-Return`, `C-M-Return` | run the query as typed, directly or in a terminal |
| `Tab`, `Shift-Tab` | complete the command or path |
| `Up`/`Down`, `C-p`/`C-n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace`, `Delete` | edit |
| `C-v` | paste |
| `Escape`, or `BackSpace` on an empty query | leave the open category |
| `Escape` with no category, or a click | close |

- **Entries** come from the plugin's desktop index. A changed
  applications dir is rescanned on the next open; the old list shows
  until the new one lands whole.
- **Running**: a typed line runs through `/bin/sh -c`. A completed name
  is inserted as one escaped word, and a name with a control or bidi
  character is never offered. `Exec=` keeps its quoting and field codes;
  `Terminal=true` runs in `terminal`. Every launch carries a fresh
  activation token (`XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID`, or a
  D-Bus `Activate`'s platform data), so a running app can raise itself.
- **Completion**: the command word from `$PATH`, or from its own dir
  when typed as a path (`./run`, `~/bin/x`). Tab takes the first match of
  the sorted scan (at most 512 names, or what 2 s gathered from a stalled
  mount).
- **Keys** repeat at the keyboard's rate. A layer that takes the keyboard
  (a power menu) closes the prompt instead of losing keys to it; so does
  losing its monitor.
- **Limits**: input totals 4 KiB of UTF-8. A paste turns newlines and
  tabs into spaces, drops other control, bidi and separator characters
  and malformed bytes, and gives up after 1.5 s. History and ranking live
  in the [state file](reference.md#state).
