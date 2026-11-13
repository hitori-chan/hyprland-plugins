# Reference

The whole command and its answers. Examples are in the
[README](../README.md); how it works and its limits are in the
[design](design.md).

## hyprctl

```sh
hyprctl vclick click  <window> <x> <y> [button] [double|triple] [glide|jump] [size:<px>]
hyprctl vclick scroll <window> <x> <y> up|down|left|right [steps] [<ms>ms] [<n>px] [glide|jump] [size:<px>]
hyprctl vclick move   <window> <x> <y> [glide|jump] [size:<px>]
hyprctl vclick press  <window> <x> <y> [button] [glide|jump] [size:<px>]
hyprctl vclick release
hyprctl vclick drag   <window> <x> <y> <x2> <y2> [button] [glide|jump] [size:<px>]
hyprctl vclick pinch  <window> <x> <y> <scale> [<deg>deg] [glide|jump] [size:<px>]
hyprctl vclick key    <window> <key> [mods]
hyprctl vclick status
hyprctl vclick where
hyprctl vclick hide
hyprctl vclick mode [mouse|tap]
hyprctl vclick profile [reload [file]]
hyprctl vclick image <window>
```

The options follow the point, in any order, each at most once.

- `<window>`: a window selector, as `send_shortcut` takes it
  (`address:0x…`, `class:…`), or a bare `0x…` address. No spaces.
- `<x> <y>`: logical pixels from the window's top-left: the pixel of a
  window capture (`grim -T <stableId>`) at scale 1; divide a capture's
  pixels by the monitor scale elsewhere. A scroll goes to what is under
  that point, as the wheel does.
- `button`: `left` (default), `right`, `middle`, `side`, `extra`, or a
  Linux code from 272 (`BTN_LEFT`) to 279.
- `double`, `triple`: two or three clicks in a row.
- `up`, `down`, `left`, `right`: the wheel's way. `down` shows what is
  below (the content moves up), whatever the user's natural-scroll
  setting.
- `steps`: wheel detents, 1 (default) to 20.
- `<ms>ms`: the time between steps, `0ms` to `1000ms` (default `50ms`).
  The first step goes at once; the rest follow at that pace
  ([design](design.md#paced-scrolling)). `0ms` sends every step in one
  burst, which many apps speed up.
- `<n>px`: instead of wheel steps, a finger's scroll on a touchpad by
  that many pixels, `1px` to `10000px`, exactly, at the user's pace; no
  steps or pace with it ([design](design.md#a-touchpads-strokes)).
- `move`: go there, and keep the hover until the user's pointer changes
  surface, vclick's next input goes into another app, `hide`, or the
  plugin unloads ([design](design.md#the-hover)). With a button held, the
  move drags it ([press](#press-and-release)).
- `glide`: go there along a curve at the user's pace, rest on the target
  (the dwell), then the input ([design](design.md#the-glide)).
  `jump`: go at once. A request that names neither goes the mode's way:
  `mouse` glides, `tap` jumps.
- `size:<px>`: the target's size, 4 to 1000 (default 32): Fitts's W for
  the glide's time, so a small button takes a little longer and a wide
  one less.

- `<key>`: an xkb key name in the active layout, as `send_shortcut`
  takes it (`Next` is PageDown, `Prior` PageUp; `Return`, `Escape`,
  `a`, `F5`; any case), or `code:<n>` (an xkb keycode, the evdev code
  plus 8). One press and its release.
- `[mods]`: the modifiers held for that key, joined by `+` (or `_`):
  `ctrl`, `shift`, `alt`, `super` (and `control`, `mod1`-`mod5`, `win`,
  `logo`, `meta`), any case. `key` takes no other option
  ([design](design.md#the-keyboard)).

The answer is `ok`, or one line starting `error:` (`usage:` for a
request that names no verb). A glide answers `ok` as it sets off; the
click or scroll follows when it arrives, and an error there shows in
`status`.

A glide starts where vclick's pointer last was in that window, else at
the window's centre. It goes as a jump instead when the user's pointer
is in the same app (a glide would take the app's pointer from them; a
`move` is refused then) or when the point is in a popup outside the
window's box.

## Lua

Every verb is a function under `hl.plugin.vclick`, with the same words
in the same order (a number where hyprctl takes one) and the same
checks:

```lua
hl.plugin.vclick.click(window, x, y, [button], ["double"|"triple"], ["glide"|"jump"])
hl.plugin.vclick.scroll(window, x, y, "up"|"down"|"left"|"right", [steps], ["<ms>ms"], ["<n>px"], ["glide"|"jump"])
hl.plugin.vclick.move(window, x, y, ["glide"|"jump"])
hl.plugin.vclick.press(window, x, y, [button], ["glide"|"jump"])
hl.plugin.vclick.release()
hl.plugin.vclick.drag(window, x, y, x2, y2, [button], ["glide"|"jump"])
hl.plugin.vclick.pinch(window, x, y, scale, ["<deg>deg"], ["glide"|"jump"])
hl.plugin.vclick.key(window, key, [mods])
hl.plugin.vclick.hide()
hl.plugin.vclick.status()      -- "idle", "moving", "scrolling 3/12", ...
hl.plugin.vclick.where()
hl.plugin.vclick.mode(["mouse"|"tap"])
hl.plugin.vclick.profile(["reload", [file]])
hl.plugin.vclick.image(window)
```

The answer `ok` comes back as `true`; an error as `nil` and its line
(`error: …`, or `usage: …` for a call that names too little), so a
caller can `assert()` it or branch on it; any other answer as its
string. A table, a boolean or `nil` where a word goes is `error:
argument <n> is a <type>: a string or a number, as hyprctl takes it`.

```lua
local ok, err = hl.plugin.vclick.key("address:0x…", "Next")
if not ok then print(err) end
```

From a shell: `hyprctl repl 'return hl.plugin.vclick.status()'`.

## key

`key <window> <key> [mods]` sends that window one key, pressed and
released, with the modifiers held for it, and answers `ok` once it is
sent. The app hears an enter, the modifiers, the press, the release, the
modifiers released and a leave, unless the user's keyboard is in that
very window: then it hears only the key, between its modifiers. Nothing
else hears anything, and the keyboard focus stays where it is. It is
refused while the user's keyboard is in another window of the same app,
or while they hold a key in that one (`error: busy`). The key goes at
once, whatever the mode: no glide, no cursor.

## press and release

`press <window> <x> <y> [button]` goes there and holds the button down;
`release` lets it go where the pointer is. Between them, `move` in that
window drags it (a glide, or a `jump` as one motion), as a hand holding
the button down: the app's implicit grab, so every motion goes to where
the press began, wherever the point is. Nothing else is taken while it
is held (`error: busy: holding <button>; release it first`), except
`key`, the queries, `hide` and `mode`.

The hold ends by itself, and `status` says why, when:

- the user's pointer changes surface, anywhere: Hyprland's seat leaves
  every app's pointer then, and a leave lets the button go (so does the
  user's pointer coming into the app, whose pointer is theirs);
- the window closes or is hidden;
- the app begins a drag-and-drop from it: Hyprland would drive that with
  the user's real mouse, so it is refused (the app hears its drag
  cancelled; [design](design.md#a-held-button)).

`hide`, the switch to `tap` and unloading let the button go first. The
app cannot make the press an interactive move or resize of its window
(a client-side title bar): the compositor would drive those with the
user's mouse too.

## drag

`drag <window> <x> <y> <x2> <y2> [button]` is a press, a move and a
release in one request: it goes to `<x> <y>` (a glide or a jump, as any
request), presses, glides to `<x2> <y2>` with the button down at the
user's pace (Fitts's time for the target's size, never past the target),
rests there (the dwell), and lets go. `<x2> <y2>` is in the window's box.
`status` says `moving` on the way there, `dragging` after the press. The
hold's limits are the press's ([press and release](#press-and-release)):
a drag-and-drop the app begins is refused, and the user's pointer
changing surface ends it.

## pinch

`pinch <window> <x> <y> <scale> [<deg>deg]` goes to `<x> <y>` (a glide
or a jump, as any request) and pinches there with two fingers on a
touchpad: they spread to `<scale>` times their distance, `0.1` to `10`
(`2` zooms in twice, `0.5` out), and turn `<deg>` degrees, `-360` to
`360`, clockwise positive; at the user's pace, then a rest, then the
lift ([design](design.md#a-touchpads-strokes)). A pinch needs a scale
other than 1, or a turn. `status` says `pinching`.

## status

- `moving`: a glide runs, its dwell included.
- `dragging`: a glide runs with a button held.
- `scrolling <done>/<steps>`: a paced scroll runs; `scrolling
  <done>/<n> px`: a finger's.
- `pinching`: a pinch runs.
- `idle`, then `; holding <button>` while one is held, then `; <why>`
  when the last glide, scroll or hold ended early, until the next one
  starts:
  - `the last glide stopped: <why>`: the window closed or was hidden,
    the compositor is shutting down, or an error;
    the app's pointer left the window;
  - `the last glide finished as a jump: the user's pointer came into the
    app`;
  - `the last move stopped: the user's pointer came into the app` (or
    `press`, `drag`);
  - `the hold ended: <why>`: the user's pointer changed surface or came
    into the app, the window closed or was hidden, or the app began a
    drag-and-drop;
  - `the app began a drag-and-drop as the button went up, ...; refused`;
  - `the last drag's release failed: <error>`;
  - `the last glide's input failed: <error>`;
  - `the last scroll stopped after <done>/<steps>: <error>` (or
    `<done>/<n> px: <why>`: the window closed or was hidden, the user's
    pointer came into the app);
  - `the last pinch stopped: <why>`: the same; the app heard it
    cancelled.

Nothing else is taken while a glide or a scroll runs. To wait for one:

```sh
until [[ "$(hyprctl vclick status)" == idle* ]]; do sleep 0.02; done
```

## where

Where the mouse is: `address:0x<window> <x> <y>`, with ` (fading)`
after a glide in the tap mode; else `nowhere` (also once the user's
pointer came into the app: the app's pointer is theirs, and the next
input there starts where their pointer left the window).

## hide

Takes the mouse away: a held button goes up, the cursor goes and the
app's pointer leaves, as a hand lifting off the mouse. `ok`, or `error: busy` while a glide or a
scroll runs. The next input brings it back.

## mode

`mode` answers `mouse` or `tap`; `mode mouse|tap` sets it. It is
`mouse` whenever the plugin loads ([design](design.md#the-modes)).

- `mouse`: a request glides unless it says `jump`; after it, the app's
  pointer and the cursor stay where it went.
- `tap`: a request jumps unless it says `glide`; after it, the app's
  pointer leaves and no cursor stays (a held button's shows until the
  release, then fades). Switching to `tap` does `hide` (unless a glide
  or a scroll runs).

## profile

`profile` answers the pace the glide moves with, then where it came
from:

```
fitts a 114 ms, b 75 ms/bit (W 32 px); bow 0.0821 +- 0.0904; speed peak at 0.319; overshoot 12.3% of moves >= 300 px, by 6.02%; dwell 237 ms; 156 Hz
from /home/user/.local/state/vclick/profile (400 movements)
```

`profile reload [file]` reads the file again (by default
`$XDG_STATE_HOME/vclick/profile`, else `~/.local/state/vclick/profile`)
and answers `loaded <file>`, or `no profile at <file>: the built-in
defaults`, then the profile. The plugin reads the default file when it
loads.

### The profile file

`key=value` lines, written by `learn.py fit`; `#` starts a comment. A
missing key keeps its default; an unknown key or a bad value is skipped;
a value outside its bounds is clamped.

| Key | Meaning | Bounds | Default |
|---|---|---|---|
| `fitts_a_ms`, `fitts_b_ms` | Fitts's law: a move takes a + b · log2(D / 32 + 1) ms | 0–400, 30–400 | 50, 120 |
| `bow_mean`, `bow_sd` | how far a path bows sideways, over its length | 0–0.25, 0–0.2 | 0.06, 0.04 |
| `peak` | where the speed peaks, 0–1 of the move | 0.3–0.7 | 0.45 |
| `overshoot_p`, `overshoot` | how often a move of 300 px or more overshoots, and by how much of D | 0–1, 0–0.15 | 0.3, 0.03 |
| `dwell_ms` | the rest on the target before a click or scroll | 0–400 | 80 |
| `rate_hz` | motion events a second | 30–1000 | 125 |
| `moves` | how many moves it was fitted from | | 0 |

## image

`image <window>` answers what vclick draws as the cursor over that
window ([design](design.md#the-cursor-image)):

- `the app's image, <w>x<h>, hotspot <x>,<y>`: the image the app last
  set (`wl_pointer.set_cursor`);
- `the theme's '<shape>', <w>x<h>, hotspot <x>,<y>`: the shape it named
  (`cursor-shape-v1`), from the XCursor theme, in logical px;
- `nothing: the app hid its cursor`;
- `the built-in arrow: no '<shape>' in the cursor theme`.

`; the app has named none yet` follows when the app has asked for no
cursor (the theme's `default` stands in). ` (no set_cursor hook: no app
images)` follows when the hook could not be installed.

## Errors

- `error: bad coordinates '<x> <y>'`, `error: '<word>' is not an
  option of click|scroll|move|press|drag|pinch`, `error: '<word>': that option is given
  twice`, `error: a scroll needs its direction: up, down, left or
  right`, `error: '<n>': steps must be 1-20`, `error: '<n>ms': the pace
  must be 0ms-1000ms`, `error: 'size:<n>': the target's size must be
  4-1000 px`, `error: a drag needs where it lets go: <x2> <y2>`,
  `error: '<n>px': a scroll by px must be 1px-10000px`, `error: a scroll
  by px takes no steps or pace: a finger's goes at the user's`, `error:
  a pinch needs its scale: 0.1-10 (2: twice as large)`, `error: a pinch
  needs a scale other than 1, or a turn`, `error: '<n>deg': a turn must
  be -360deg-360deg`: a malformed request; the usage follows. `error: <x2>,<y2> is outside
  the window (<w>x<h>)`: a drag's end.
- `error: busy: still moving; retry later`, `error: busy: still
  dragging; retry later`, `error: busy: still scrolling <done>/<steps>;
  retry later` (or `<done>/<n> px`), `error: busy: still pinching; retry
  later`: a glide, a scroll or a pinch is running.
- `error: busy: holding <button>; release it first`, `error: busy:
  holding <button> in another window; release it first`: a request a
  held button doesn't allow. `error: no button is held`: a `release`
  with none.
- `error: no drag-and-drop guard (the initiateDrag hook is out): a held
  button could hand a drag to the user's mouse`: a `press` after a
  compositor change took the hook's symbol away.
- `error: busy: the user's pointer is pressed, constrained or dragging
  in this app; retry later`: the user's mouse is in the same app and
  holds a button, a pointer constraint or a drag there
  ([design](design.md#the-same-app)). Input into other apps goes
  through meanwhile.
- `error: busy: the user's pointer is in this app; a move would take it
  from them` (or `a press`, `a drag`, `a scroll by px`, `a pinch`): one
  into the app under the user's mouse. `error: the app takes no pinch (it
  bound no zwp_pointer_gesture_pinch_v1)`.
- `error: '<key>' is not a key`, `error: '<word>' is not a modifier`: a
  malformed `key`; the usage follows. `error: no keyboard: the seat has
  none to take the keymap from`: the compositor has no keyboard.
- `error: busy: the user's keyboard is in another window of this app;
  retry later`, `error: busy: the user holds a key in this app; retry
  later`: a `key` into the app with the user's keyboard
  ([design](design.md#the-keyboard)).
- `error: the app has no keyboard (it bound no wl_keyboard)`.
- `error: XWayland windows are not supported`: the compositor's hit test
  asserts on them.
- `error: window not found`, `error: the window is not mapped` (hidden
  or unmapped), `error: <x>,<y> is outside the window (<w>x<h>)` (unless
  one of its popups is there), `error: the window has no surface`,
  `error: the app has no pointer (it bound no wl_pointer)`.
- `error: the compositor is shutting down`. The lockscreen refuses
  nothing ([design](design.md#under-the-lockscreen)).
