# Reference

The whole command and its answers. Examples: the [README](../README.md);
how it works and its limits: the [design](design.md).

## hyprctl

```sh
hyprctl vclick click  <window> <x> <y> [button] [double|triple] [human|performance] [size:<px>]
hyprctl vclick scroll <window> <x> <y> up|down|left|right [steps] [<ms>ms] [<n>px] [human|performance] [size:<px>]
hyprctl vclick move   <window> <x> <y> [human|performance] [size:<px>]
hyprctl vclick press  <window> <x> <y> [button] [human|performance] [size:<px>]
hyprctl vclick release
hyprctl vclick drag   <window> <x> <y> <x2> <y2> [button] [human|performance] [size:<px>]
hyprctl vclick pinch  <window> <x> <y> <scale> [<deg>deg] [human|performance] [size:<px>]
hyprctl vclick key    <window> <key> [mods]
hyprctl vclick status
hyprctl vclick where
hyprctl vclick hide
hyprctl vclick mode [human|performance]
hyprctl vclick profile [reload [file]]
hyprctl vclick image <window>
```

Options follow the point, in any order, each at most once.

- `<window>`: a window selector as `send_shortcut` takes it
  (`address:0x…`, `class:…`), or a bare `0x…` address. No spaces.
- `<x> <y>`: logical pixels from the window's top-left: a window
  capture's pixel (`grim -T <stableId>`) at scale 1, else a capture's
  pixels over the monitor scale. A scroll goes to what is under the
  point, as the wheel does.
- `button`: `left` (default), `right`, `middle`, `side`, `extra`, or a
  Linux code 272 (`BTN_LEFT`) to 279.
- `double`, `triple`: two or three clicks in a row.
- `up`, `down`, `left`, `right`: the wheel's way. `down` shows what is
  below, whatever the user's natural-scroll setting.
- `steps`: wheel detents, 1 (default) to 20.
- `<ms>ms`: the time between steps, `0ms` to `1000ms`; without it,
  `wheel_ms` in `human`, `50ms` in `performance`. The first step goes at
  once ([design](design.md#paced-scrolling)); `0ms` sends all in one
  burst, which many apps speed up.
- `<n>px`: a finger's scroll on a touchpad instead, `1px` to `10000px`,
  exact; no steps or pace with it
  ([design](design.md#a-touchpads-strokes)).
- `human`: glide there at the user's pace, rest, then the input
  ([design](design.md#the-glide)). `performance`: as fast as apps still
  read it right ([design](design.md#the-modes)). Neither: the mode's.
- `size:<px>`: the target's size, 4 to 1000 (default 32): Fitts's W for
  the glide's time.
- `<key>`: an xkb key name in the active layout, as `send_shortcut` takes
  it (`Next` is PageDown, `Prior` PageUp; `Return`, `Escape`, `a`, `F5`;
  any case), or `code:<n>` (an xkb keycode: evdev plus 8).
- `[mods]`: modifiers held for the key, joined by `+` or `_`: `ctrl`,
  `shift`, `alt`, `super` (and `control`, `mod1`–`mod5`, `win`, `logo`,
  `meta`), any case. `key` takes no other option.

The answer is `ok`, or one line starting `error:` (`usage:` when no verb
is named). A glide answers `ok` as it sets off; an error at arrival shows
in `status`.

A glide starts where vclick's pointer last was in that window, else at
its centre. It goes at once instead when the user's pointer is in the
same app (a `move` is refused then) or the point is in a popup outside
the window's box.

## Lua

Every verb is a function under `hl.plugin.vclick`, with the same words in
the same order (a number where hyprctl takes one) and the same checks:

```lua
hl.plugin.vclick.click(window, x, y, [button], ["double"|"triple"], ["human"|"performance"])
hl.plugin.vclick.scroll(window, x, y, "up"|"down"|"left"|"right", [steps], ["<ms>ms"], ["<n>px"], ["human"|"performance"])
hl.plugin.vclick.move(window, x, y, ["human"|"performance"])
hl.plugin.vclick.press(window, x, y, [button], ["human"|"performance"])
hl.plugin.vclick.release()
hl.plugin.vclick.drag(window, x, y, x2, y2, [button], ["human"|"performance"])
hl.plugin.vclick.pinch(window, x, y, scale, ["<deg>deg"], ["human"|"performance"])
hl.plugin.vclick.key(window, key, [mods])
hl.plugin.vclick.hide()
hl.plugin.vclick.status()      -- "idle", "moving", "scrolling 3/12", ...
hl.plugin.vclick.where()
hl.plugin.vclick.mode(["human"|"performance"])
hl.plugin.vclick.profile(["reload", [file]])
hl.plugin.vclick.image(window)
```

`ok` returns `true`; an error returns `nil` and its line (`error: …`, or
`usage: …`); any other answer its string. A table, boolean or `nil` where
a word goes is `error: argument <n> is a <type>: a string or a number, as
hyprctl takes it`.

```lua
local ok, err = hl.plugin.vclick.key("address:0x…", "Next")
if not ok then print(err) end
```

From a shell: `hyprctl repl 'return hl.plugin.vclick.status()'`.

## key

Sends the window one key, pressed and released with its modifiers, and
answers `ok` once sent: an enter, the modifiers, the press, the release,
the modifiers released, a leave; only the key, between its modifiers,
when the user's keyboard is in that very window. The keyboard focus stays
where it is. `error: busy` while the user's keyboard is in another window
of the same app or holds a key in that one. No glide, no cursor, either
mode ([design](design.md#the-keyboard)).

## press and release

`press` goes there and holds the button; `release` lets it go where the
pointer is. Between them a `move` in that window drags it (a glide, or
one motion in `performance`) under the app's implicit grab: every motion
goes to where the press began. Only `key`, the queries, `hide` and `mode`
are taken meanwhile (`error: busy: holding <button>; release it first`).

The hold ends by itself, and `status` says why, when:

- the user's pointer changes surface anywhere (the seat leaves every
  app's pointer, and a leave releases), or comes into the app;
- the window closes or is hidden;
- the app begins a drag-and-drop from it: refused, the app hears it
  cancelled ([design](design.md#a-held-button)).

`hide`, the switch to `performance` and unloading release first. The app
cannot turn the press into an interactive move or resize.

## drag

`drag <window> <x> <y> <x2> <y2> [button]`: press at `<x> <y>`, move to
`<x2> <y2>` (inside the window's box) with the button down, release. In
`human` a glide at the user's pace, never past the target, then a rest;
in `performance` straight there in 48 ms. `status` says `moving`, then
`dragging` after the press. Its limits are the
[press's](#press-and-release).

## pinch

`pinch <window> <x> <y> <scale> [<deg>deg]`: two fingers on a touchpad
spread to `<scale>` times their distance, `0.1` to `10` (`2` zooms in),
and turn `<deg>`, `-360` to `360`, clockwise positive; at the user's pace,
a rest, the lift; in `performance` 32 ms, no rest
([design](design.md#a-touchpads-strokes)). It needs a scale other than 1,
or a turn. `status` says `pinching`.

## status

- `moving`: a glide runs, its rest included.
- `clicking`: a human click holds its button or waits for its next click.
- `dragging`: a glide runs with a button held.
- `scrolling <done>/<steps>`, `scrolling <done>/<n> px`: a paced or a
  finger's scroll runs.
- `pinching`: a pinch runs.
- `idle`, then `; holding <button>` while one is held, then `; <why>`
  when the last glide, scroll or hold ended early, until the next starts:
  - `the last glide stopped: <why>`: the window closed or was hidden, the
    compositor is shutting down, or an error; the app's pointer left;
  - `the last glide finished at once: the user's pointer came into the
    app`;
  - `the last move stopped: the user's pointer came into the app` (or
    `press`, `drag`);
  - `the hold ended: <why>`: the user's pointer changed surface or came
    into the app, the window closed or was hidden, or the app began a
    drag-and-drop;
  - `the app began a drag-and-drop as the button went up, ...; refused`;
  - `the last drag's release failed: <error>`;
  - `the last glide's input failed: <error>`;
  - `the last scroll stopped after <done>/<steps>: <error>` (or `<done>/<n>
    px: <why>`: the window closed or was hidden, the user's pointer came
    into the app);
  - `the last pinch stopped: <why>`: the same; the app heard it cancelled.

Nothing else is taken while a glide or scroll runs. To wait:

```sh
until [[ "$(hyprctl vclick status)" == idle* ]]; do sleep 0.02; done
```

## where

`address:0x<window> <x> <y>`, with ` (fading)` after a performance glide;
else `nowhere` (also once the user's pointer came into the app; the next
input there starts where it left the window).

## hide

Lifts the hand: a held button goes up, the cursor goes, the app's pointer
leaves. `ok`, or `error: busy` while a glide or scroll runs. The next
input brings it back.

## mode

`mode` answers `human` or `performance`; `mode human|performance` sets
it; `human` at every load ([design](design.md#the-modes)).

- `human`: requests go at the user's pace unless they say `performance`;
  the app's pointer and the cursor stay after.
- `performance`: requests go at once unless they say `human`; the app's
  pointer leaves after and no cursor stays (a held button's shows until
  the release, then fades). Switching to it does `hide` unless a glide or
  scroll runs.

## profile

`profile` answers the glide's pace, then its source:

```
stroke a 94 ms, b 23 ms/bit (W 32 px); bell 4 peaking at 0.41; bow 0.0526 (sigma 0.757, slope 0.233), 20.3% both ways; drift 32.1%, 472 ms, 14.9% of the way; corrected 14.1%, 66.7% from short, by 10.11%; tail on 49.3% of moves at 300 px (+12.1 a doubling), 46.7% of the time, 8.8% of the way; rest 97 ms (sigma 0.875); hold 89 ms (sigma 0.219), 106 ms between clicks; drag lead 107 ms, drop rest 65 ms; wheel 50 ms; 129 Hz
from /home/user/.local/state/vclick/profile (194 movements)
```

`profile reload [file]` rereads the file (default
`$XDG_STATE_HOME/vclick/profile`, else `~/.local/state/vclick/profile`)
and answers `loaded <file>` or `no profile at <file>: the built-in
defaults`, then the profile. The default file is read at load.

### The profile file

`key=value` lines from `learn.py fit`; `#` starts a comment. A missing
key keeps its default, an unknown key or bad value is skipped, a value
out of bounds is clamped.

| Key | Meaning | Bounds | Default |
|---|---|---|---|
| `stroke_a_ms`, `stroke_b_ms` | Fitts's law: a first stroke takes a + b · log2(D / W + 1) ms | 0–400, 10–400 | 50, 120 |
| `bell` | speed t^α (1 − t)^β, α + β = `bell` | 2–24 | 4 |
| `peak` | where the speed peaks, of the move | 0.2–0.7 | 0.45 |
| `bow_median`, `bow_sigma` | a 300 px path's sideways bow, of its length (log-normal) | 0–0.25, 0–1.5 | 0.05, 0.8 |
| `bow_slope` | the bow grows as (D / 300 px) to this power | −0.5–1 | 0 |
| `lead_p`, `lead_ms`, `lead_size` | share of moves ≥ 300 px that drift first, how long, how far of the way | 0–0.6, 100–1000, 0–0.7 | 0, 300, 0.15 |
| `s_share` | share of moves ≥ 100 px that bend both ways | 0–0.6 | 0.15 |
| `correct_p`, `under_share` | how often a small correction follows, how often from short | 0–1, 0–1 | 0.05, 0.6 |
| `correct_size` | how far off the first stroke lands, of D | 0–0.15 | 0.05 |
| `tail_p`, `tail_p_slope` | share of 300 px moves that end slowly, change per doubling of D | 0–1, −1–1 | 1, 0 |
| `tail_share`, `tail_size` | a slow end on top of the stroke: its share of the time, of the way | 0–0.8, 0–0.2 | 0, 0.03 |
| `rest_ms`, `rest_sigma` | arrival within 3 px to the click or scroll (log-normal) | 0–600, 0–1.5 | 120, 0.6 |
| `hold_ms`, `hold_sigma` | a click's hold (log-normal) | 20–250, 0–1 | 100, 0.25 |
| `double_gap_ms` | a click's release to the next press | 20–250 | 120 |
| `drag_lead_ms`, `drop_rest_ms` | press until 3 px away; within 3 px of the drop until release | 0–400, 0–400 | 100, 80 |
| `wheel_ms` | between wheel steps unless the request names a pace | 10–200 | 50 |
| `rate_hz` | motion events a second | 30–1000 | 125 |
| `moves` | moves fitted from | | 0 |

## image

What vclick draws as the cursor over that window
([design](design.md#the-cursor-image)):

- `the app's image, <w>x<h>, hotspot <x>,<y>`: the app's last
  `wl_pointer.set_cursor`;
- `the theme's '<shape>', <w>x<h>, hotspot <x>,<y>`: its named
  `cursor-shape-v1` shape from the XCursor theme, in logical px;
- `nothing: the app hid its cursor`;
- `the built-in arrow: no '<shape>' in the cursor theme`.

Then `; the app has named none yet` when it asked for no cursor (the
theme's `default` stands in), and ` (no set_cursor hook: no app images)`
when the hook is out.

## Errors

- Malformed, followed by the usage: `error: bad coordinates '<x> <y>'`,
  `error: '<word>' is not an option of click|scroll|move|press|drag|pinch`,
  `error: '<word>': that option is given twice`, `error: a scroll needs
  its direction: up, down, left or right`, `error: '<n>': steps must be
  1-20`, `error: '<n>ms': the pace must be 0ms-1000ms`, `error:
  'size:<n>': the target's size must be 4-1000 px`, `error: a drag needs
  where it lets go: <x2> <y2>`, `error: '<n>px': a scroll by px must be
  1px-10000px`, `error: a scroll by px takes no steps or pace: a finger's
  goes at the user's`, `error: a pinch needs its scale: 0.1-10 (2: twice
  as large)`, `error: a pinch needs a scale other than 1, or a turn`,
  `error: '<n>deg': a turn must be -360deg-360deg`. A drag's end outside:
  `error: <x2>,<y2> is outside the window (<w>x<h>)`.
- Running: `error: busy: still moving; retry later`, `error: busy: still
  dragging; retry later`, `error: busy: still scrolling <done>/<steps>;
  retry later` (or `<done>/<n> px`), `error: busy: still pinching; retry
  later`.
- Held: `error: busy: holding <button>; release it first`, `error: busy:
  holding <button> in another window; release it first`; `error: no
  button is held` (a `release` with none).
- `error: no drag-and-drop guard (the initiateDrag hook is out): a held
  button could hand a drag to the user's mouse`: a `press` without the
  hook.
- `error: busy: the user's pointer is pressed, constrained or dragging in
  this app; retry later` ([design](design.md#the-same-app)); other apps
  still take input.
- `error: busy: the user's pointer is in this app; a move would take it
  from them` (or `a press`, `a drag`, `a scroll by px`, `a pinch`).
  `error: the app takes no pinch (it bound no zwp_pointer_gesture_pinch_v1)`.
- `key`: `error: '<key>' is not a key`, `error: '<word>' is not a
  modifier` (the usage follows); `error: no keyboard: the seat has none
  to take the keymap from`; `error: busy: the user's keyboard is in
  another window of this app; retry later`, `error: busy: the user holds
  a key in this app; retry later`; `error: the app has no keyboard (it
  bound no wl_keyboard)`.
- `error: XWayland windows are not supported`.
- `error: window not found`, `error: the window is not mapped`, `error:
  <x>,<y> is outside the window (<w>x<h>)` (unless a popup is there),
  `error: the window has no surface`, `error: the app has no pointer (it
  bound no wl_pointer)`.
- `error: the compositor is shutting down`. The lockscreen refuses
  nothing ([design](design.md#under-the-lockscreen)).
