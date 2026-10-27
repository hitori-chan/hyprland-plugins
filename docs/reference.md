# Reference

## Lua API

Every function lives under `hl.plugin.awesome`. Bind them as you would
any Lua function:

```lua
hl.bind("SUPER + M", function() hl.plugin.awesome.maximize() end)
```

| Function | Action |
|---|---|
| `menubar()` | open the [launcher](shell.md#the-launcher) |
| `maximize()` | toggle the focused window's maximize |
| `minimize()` | minimize the focused window |
| `restore()` | restore the most recently minimized window on the focused monitor's workspace |
| `focus_next()`, `focus_prev()` | cycle the focus in arrival order |
| `focus_prev_here()` | toggle between the two most recent windows on this workspace |
| `center()` | toggle the [center](notify.md#the-center) |
| `suspend([minutes])` | toggle Do Not Disturb; with minutes, hold it that long from now (0 ends it, `math.huge` never does) |
| `clear_all()` | dismiss every live card |
| `volume_up()`, `volume_down()`, `mute()` | the default output |
| `mic_mute()` | the default input |
| `brightness_up()`, `brightness_down()` | the display's backlight |
| `keyboard_light_up()`, `keyboard_light_down()` | the keyboard's light |
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
| `trace` | the last 32 button, scroll and key events and who took each |

`trace` names a module, `blocked` (the session lock or an input
capture), `upstream` (cancelled before the plugin) or `-` (passed to the
compositor). A run of scroll events on one axis is one entry with its
count. It is the first stop when a click goes missing.

The gate build adds `cells`, `shown`, `menu`, `quiet`, `controls`,
`outputs`, `busy` and `pad NAME` ([testing](testing.md)).

## Configuration

45 keys, `plugin:awesome:<module>:<key>`, declared in
[`core/schema.hpp`](../awesome/core/schema.hpp). Out-of-range values
clamp. In Lua:

```lua
hl.config({ plugin = { awesome = {
    shell  = { terminal = "foot", height = 28 },
    notify = { timeout_normal = 8000 },
} } })
```

Colours marked Frost default to the Frost palette
([`core/tokens.hpp`](../awesome/core/tokens.hpp)), so an empty config
already looks right.

### shell

| Key | Default | |
|---|---|---|
| `height` | 28 | bar height, logical px (reserve it as the monitor's reserved top) |
| `font_size` | 12 | text size, logical px |
| `tray_spacing` | 20 | px between tray icons |
| `font` | `Fira Code, IBM Plex Sans JP` | font family or Pango family list (the kanji tags take the first face that has them) |
| `terminal` | `alacritty` | runs the launcher's `Terminal=true` entries |
| `col_bg` | Frost | the bar's and the launcher's glass (the alpha is the glass) |
| `col_menu_bg` | Frost | menus' thicker glass |
| `col_fg` | Frost | bar text and glyphs: the clock, the bell, symbolic tray icons, occupied tags |
| `col_focus` | Frost | the launcher's and the menus' selection |
| `col_active` | Frost | the viewed tag, the focused task, the bell while the center is open |
| `col_dot` | Frost | the bell's dot |
| `col_empty` | Frost | empty tags, minimized tasks, disabled rows |
| `col_urgent` | Frost | urgent tags and tasks |
| `col_frame` | Frost | the glass rim |
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
| `offset_y` | 40 | the cards' and capsules' distance from the monitor's top |
| `timeout_low` | 4000 | ms for low-urgency, transient and progress cards |
| `timeout_normal` | 5000 | ms before a banner retreats to the center; 0 = sticky |
| `coalesce_popups` | 3 | live banners per app (0 = no cap) |
| `max_notifs` | 50 | cards kept; past it the oldest non-critical goes |
| `ignore_dbusclose` | 0 | ignore apps' `CloseNotification` (dunst's knob) |
| `rounding` | 18 | card radius, logical px |
| `rounding_power` | 3.0 | the corners' superellipse exponent |
| `sound_command` | `canberra-gtk-play` | the player for sound hints; empty disables |
| `fallback_icon_dir` | empty | iconless cards draw a random face from here |
| `col_bg` | Frost | the cards' glass (the alpha is the glass) |
| `col_menu_bg` | Frost | the card menu's thicker glass |
| `col_fg` | Frost | body text |
| `col_title` | Frost | titles, a stack header's chips, the capsules' words |
| `col_kicker` | Frost | meta and secondary text |
| `col_frame` | Frost | the glass rim |
| `col_urgent` | Frost | critical: its meta, its progress, an alarmed capsule |
| `col_highlight` | Frost | the accent: progress, the selection, the reply field |
| `col_link` | Frost | body links |

### windows

| Key | Default | |
|---|---|---|
| `edge` | 16 | distance from an output edge that arms a snap zone |
| `snap_distance` | 8 | magnetic pull distance |
| `col_frame` | Frost | the snap preview's outline (its fill is the same colour at 10 %) |

## State

What the plugin remembers lives in one file,
`$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv`: tab-separated rows,
the key last. The config files stay yours: the plugin never writes them,
and nothing is both a config key and a remembered row.

| Row | Meaning |
|---|---|
| `spot <x> <y> <class>` | where the app's last window closed, relative to its monitor |
| `windowed <x> <y> <w> <h> <class>` | its last windowed box: the restore of a window born maximized |
| `launches <count> <name>` | the launcher's ranking, by `.desktop` name |
| `history <query>` | the launcher's history, newest last |
| `notify-app <flags> <label> <app>` | an app's banners (1) or sounds (2) turned off |
| `notify-mute <until> <label> <chat>` | a chat muted until a unix time |
| `option <value> <name>` | a menu choice, written only when off its default |

Options ([`core/options.hpp`](../awesome/core/options.hpp)):

| Name | Default | Value |
|---|---|---|
| `dnd` | 0 | Do Not Disturb's end as a unix time; -1 until turned off |
| `quiet-fullscreen` | 0 | quiet over a fullscreen app |
| `quiet-sharing` | 0 | quiet while sharing the screen |
| `kbd-follows-room` | 0 | the keyboard light follows the room |
| `kbd-level` | 0 | the keyboard light's last chosen level, % (0: none yet, 50 % is used) |

- A label is display text for the bell's Silenced Apps (128 bytes).
- Each kind is bounded (1024 keys, 50 history queries); at the bound the
  least recently remembered row goes, never a new one.
- Writes are atomic (temp file and rename) and synced on a worker thread,
  so a slow disk never stalls the compositor. The windows still open at
  a logout are recorded as closing.
- Every row is checked on load and a bad one is skipped. A file that
  exists but cannot be read blocks every write (a notification says so):
  a session that could not see your state never replaces it.
