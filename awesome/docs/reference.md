# Reference

## Lua API

Every function lives under `hl.plugin.awesome`:

```lua
hl.bind("SUPER + M", function() hl.plugin.awesome.maximize() end)
```

| Function | Action |
|---|---|
| `menubar()` | open the [launcher](shell.md#the-launcher) |
| `maximize()` | toggle the focused window's maximize |
| `minimize()` | minimize the focused window |
| `restore()` | restore the most recently minimized window on the focused monitor's workspace |
| `minimized()` | the plugin's minimized windows: an array of addresses (`0x…`, as `hyprctl clients` prints them), most recent last |
| `unminimize(address)` | show that window again where it is, unraised, unfocused, no workspace change, on the next loop turn; `ok` once queued, else `not minimized`, `no such window`, `locked` or the usage |
| `focus_next()`, `focus_prev()` | cycle the focus in arrival order |
| `focus_prev_here()` | toggle between the two most recent windows on this workspace |
| `center()` | toggle the [center](notify.md#the-center) |
| `suspend([minutes])` | toggle [Do Not Disturb](notify.md#do-not-disturb); with minutes, hold it that long from now (0 ends it, `math.huge` never does) |
| `clear_all()` | dismiss every live card |
| `volume_up()`, `volume_down()`, `mute()` | the default output |
| `mic_mute()` | the default input |
| `brightness_up()`, `brightness_down()` | the display's backlight |
| `touchpad_toggle()` | flip the touchpad by hand |

## hyprctl

`hyprctl awesome <verb>`:

| Verb | Answer |
|---|---|
| `count` | the number of live cards (a lock screen's bell can read it) |
| `center` | toggles the center; `ok` |
| `state` | `center:0 live:N dnd:0` |
| `badge` | `banners:N resident:N`: what the center lists |
| `topline` | the newest card's one-line form |
| `clear` | dismisses every card, the parked ones too; `ok` |
| `pad` | the touchpad: `on`, `off` or `none` |
| `minimized` | the plugin's minimized windows, one address a line, most recent last |
| `unminimize <address>` | as the Lua `unminimize`; `ok`, `not minimized`, `no such window` or `locked` |
| `trace` | the last 32 button, scroll and key events and who took each |

`trace` names a module, `blocked` (session lock or input capture),
`upstream` (cancelled before the plugin) or `-` (passed on); a run of
scrolls on one axis is one entry with a count. It is the first stop when
a click goes missing. The gate build adds verbs
([testing](testing.md#scripts)).

## Configuration

45 keys, `plugin:awesome:<group>:<key>` (a module, or `theme` for the
shared material), declared in [`core/schema.hpp`](../core/schema.hpp).
Out-of-range values clamp. In Lua:

```lua
hl.config({ plugin = { awesome = {
    shell  = { terminal = "foot", height = 28 },
    system = { volume_step = 2, brightness_step = 10 },
    notify = { timeout_normal = 8000 },
} } })
```

Colours marked Frost default to the Frost palette
([`core/tokens.hpp`](../core/tokens.hpp)).

### theme

| Key | Default | |
|---|---|---|
| `col_glass` | Frost | the glass of the bar, cards, the center, menus and the launcher: a tint over `decoration:blur` (Frost's has none; its alpha 0 keeps the RGB as the glass's dark) |
| `col_rim` | Frost | the glass rim |

### shell

| Key | Default | |
|---|---|---|
| `height` | 28 | bar height, logical px; the bar reserves it |
| `font_size` | 12 | text size, logical px |
| `tray_spacing` | 15 | px between tray icons |
| `font` | `Fira Code, IBM Plex Sans JP` | font family or Pango family list (the kanji tags take the first face that has them) |
| `terminal` | `foot` | runs the launcher's `Terminal=true` entries |
| `clock_format` | `%a %b %-d %H:%M` | the clock's `strftime` format in the session's `LC_TIME`, redrawn each minute; an empty or overlong result shows the default |
| `col_fg` | Frost | bar text and glyphs: the clock, the bell, symbolic tray icons, occupied tags |
| `col_focus` | Frost | the launcher's pick and caret, the client list's focused window |
| `col_active` | Frost | the viewed tag and the focused task (text) |
| `col_dot` | Frost | the bell's dot |
| `col_empty` | Frost | empty tags, minimized tasks, disabled rows |
| `col_urgent` | Frost | urgent tags and tasks |
| `col_charging` | Frost | the battery charging, or held at its charge limit |
| `col_low` | Frost | the battery at 20 % or below |
| `col_powersave` | Frost | the battery in power save |

### notify

| Key | Default | |
|---|---|---|
| `font` | `Inter` | font family |
| `font_size` | 13 | body text size, logical px (the other text sizes derive from it) |
| `width` | 356 | card width, logical px |
| `max_height` | 320 | banner height cap, logical px |
| `max_icon` | 36 | the icon column, logical px |
| `margin` | 8 | gap between cards, logical px |
| `offset_y` | 40 | the cards' and capsules' distance from the monitor's top, at least 12 px under the bar |
| `timeout_low` | 4000 | ms for low-urgency, transient and progress cards |
| `timeout_normal` | 5000 | ms before a banner retreats to the center; 0 = sticky |
| `coalesce_popups` | 3 | live banners per app (0 = no cap) |
| `max_notifs` | 50 | cards kept; past it the oldest non-critical goes |
| `ignore_dbusclose` | 0 | ignore apps' `CloseNotification` (dunst's knob) |
| `rounding` | 18 | card radius, logical px |
| `rounding_power` | 3.0 | the corners' superellipse exponent |
| `sound_command` | `canberra-gtk-play` | the player for sound hints; empty disables |
| `fallback_icon_dir` | empty | iconless cards draw a random face from here |
| `col_fg` | Frost | body text |
| `col_title` | Frost | titles, a stack header's chips, the capsules' words |
| `col_kicker` | Frost | meta and secondary text |
| `col_urgent` | Frost | critical: its meta, its progress, an alarmed capsule |
| `col_highlight` | Frost | the accent: progress, the selection, the reply field |
| `col_link` | Frost | body links |

### windows

| Key | Default | |
|---|---|---|
| `edge` | 16 | distance from an output edge that arms a snap zone |
| `snap_distance` | 8 | magnetic pull distance |
| `col_frame` | Frost | the snap preview's outline (its fill is the same colour at 10 %) |

### system

| Key | Default | |
|---|---|---|
| `volume_step` | 5 | percent a volume key moves the default sink |
| `volume_max` | 100 | percent the volume keys raise to; a level set past it elsewhere is kept |
| `brightness_step` | 5 | percent of the backlight's range a brightness key moves |

## State

One file, `$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv`:
tab-separated rows, the key last. The plugin never writes config files,
and nothing is both a config key and a row.

| Row | Meaning |
|---|---|
| `spot <x> <y> <class>` | where the app's last window closed, relative to its monitor |
| `windowed <x> <y> <w> <h> <class>` | its last windowed box: the restore of a window born maximized |
| `launches <count> <name>` | the launcher's ranking, by `.desktop` name |
| `history <query>` | the launcher's history, newest last |
| `notify-app <flags> <label> <app>` | an app's banners (1) or sounds (2) turned off |
| `notify-mute <until> <label> <chat>` | a chat muted until a unix time |
| `option <value> <name>` | a menu choice, written only when off its default |

Options ([`core/options.hpp`](../core/options.hpp)):

| Name | Default | Value |
|---|---|---|
| `dnd` | 0 | Do Not Disturb's end as a unix time; -1 until turned off |
| `quiet-fullscreen` | 0 | quiet over a fullscreen app |
| `quiet-sharing` | 0 | quiet while sharing the screen |

- A label is the bell's display text (128 bytes).
- Each kind is bounded (1024 keys, 50 history queries); at the bound the
  least recently remembered row goes.
- Writes are atomic and synced on a worker thread. Windows open at
  logout are recorded as closing.
- Bad rows are skipped on load. A file that exists but cannot be read
  blocks every write (a notification says so), so a session that could
  not see your state never replaces it.
