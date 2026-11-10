# Reference

The whole command and its answers. Examples are in the
[README](../README.md); how it works and its limits are in the
[design](design.md).

## hyprctl

```sh
hyprctl vclick <window> <x> <y> [button [clicks]] [glide|jump]
hyprctl vclick <window> <x> <y> scroll-up|scroll-down|scroll-left|scroll-right [steps [ms]] [glide|jump]
hyprctl vclick <window> <x> <y> move [glide|jump]
hyprctl vclick status
hyprctl vclick mode [glide|jump]
hyprctl vclick profile [reload [file]]
hyprctl vclick cursor <window>
```

- `<window>`: a window selector, as `send_shortcut` takes it
  (`address:0x…`). No spaces.
- `<x> <y>`: logical pixels from the window's top-left: the pixel of a
  window capture (`grim -T <stableId>`) at scale 1; divide a capture's
  pixels by the monitor scale elsewhere. A scroll goes to what is under
  that point, as the wheel does.
- `button`: `left` (default), `right`, `middle`, `side`, `extra`, or a
  Linux code from 272 (`BTN_LEFT`) to 279.
- `clicks`: 1 (default) to 3; 2 is a double click.
- `steps`: wheel detents, 1 (default) to 20. `scroll-down` shows what is
  below (the content moves up), whatever the user's natural-scroll
  setting.
- `ms`: the time between steps, 0 to 1000 (default 50). The first step
  goes at once; the rest follow at that pace
  ([design](design.md#paced-scrolling)). `0` sends every step in one
  burst, which many apps speed up.
- `move`: go there and stay. The app keeps the hover until the user's
  pointer changes surface, vclick's next input goes into another app, or
  the plugin unloads ([design](design.md#the-hover)).
- `glide`: go there along a curve at the user's pace, rest on the target
  (the dwell), then click or scroll ([design](design.md#the-glide)).
  `jump`: go at once. A request that names neither goes the mode's way.

The answer is `ok`, or one line starting `error:` (`usage:` for a
malformed request). A glide answers `ok` as it sets off; the click or
scroll follows when it arrives, and an error there shows in `status`.

A glide starts where vclick's pointer last was in that window, else at
the window's centre. It goes as a jump instead when the user's pointer
is in the same app (a glide would take the app's pointer from them; a
`move` is refused then) or when the point is in a popup outside the
window's box.

## status

- `moving`: a glide runs, its dwell included.
- `scrolling <done>/<steps>`: a paced scroll runs.
- `idle`, or `idle; <why>` when the last glide or scroll ended early,
  until the next one starts:
  - `the last glide stopped: <why>`: the window closed or was hidden,
    the session locked, the compositor is shutting down, or an error;
    the app's pointer left the window;
  - `the last glide finished as a jump: the user's pointer came into the
    app`;
  - `the last move stopped: the user's pointer came into the app`;
  - `the last glide's input failed: <error>`;
  - `the last scroll stopped after <done>/<steps>: <error>`.

Nothing else is taken while a glide or a scroll runs. To wait for one:

```sh
until [[ "$(hyprctl vclick status)" == idle* ]]; do sleep 0.02; done
```

## mode

`mode` answers `glide` or `jump`, the way a request goes when it names
neither; `mode glide|jump` sets it. It is `glide` whenever the plugin
loads.

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

## cursor

`cursor <window>` answers what a glide draws over that window:

- `the app's image, <w>x<h>, hotspot <x>,<y>`: the image the app last
  set (`wl_pointer.set_cursor`);
- `the built-in arrow: the app asked for '<shape>'`: a named shape
  (`cursor-shape-v1`) or `hidden`; vclick draws no themed cursors;
- `the built-in arrow: the app has set no cursor image yet`;
- `the built-in arrow: the app's image has no buffer`.

` (no set_cursor hook: the arrow only)` follows when the hook could not
be installed ([design](design.md#the-cursor-image)).

## Errors

- `error: bad coordinates`, `error: unknown button`, `error: count must
  be 1-3`, `error: steps must be 1-20`, `error: ms must be 0-1000`: a
  malformed request; the usage follows.
- `error: busy: still moving; retry later`, `error: busy: still
  scrolling <done>/<steps>; retry later`: a glide or a paced scroll is
  running.
- `error: busy: the user's pointer is pressed, constrained or dragging
  in this app; retry later`: the user's mouse is in the same app and
  holds a button, a pointer constraint or a drag there
  ([design](design.md#the-same-app)). Input into other apps goes
  through meanwhile.
- `error: busy: the user's pointer is in this app; a move would take it
  from them`: a `move` into the app under the user's mouse.
- `error: XWayland windows are not supported`: the compositor's hit test
  asserts on them.
- `error: window not found`, `error: the window is not mapped` (hidden
  or unmapped), `error: <x>,<y> is outside the window (<w>x<h>)` (unless
  one of its popups is there), `error: the window has no surface`,
  `error: the app has no pointer (it bound no wl_pointer)`.
- `error: the session is locked`, `error: the compositor is shutting
  down`.
