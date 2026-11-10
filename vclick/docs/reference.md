# Reference

The whole command and its answers. Examples are in the
[README](../README.md); how it works and its limits are in the
[design](design.md).

## hyprctl

```sh
hyprctl vclick <window> <x> <y> [button [clicks]]
hyprctl vclick <window> <x> <y> scroll-up|scroll-down|scroll-left|scroll-right [steps [ms]]
hyprctl vclick status
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
  goes at once, and the answer comes with it; the rest follow at that
  pace ([design](design.md#paced-scrolling)). `0` sends every step in
  one burst, which many apps speed up.

The answer is `ok`, or one line starting `error:` (`usage:` for a
malformed request).

`status` answers `scrolling <done>/<steps>` while a paced scroll runs,
then `idle`. If a scroll stopped early, it answers `idle; the last
scroll stopped after <done>/<steps>: <error>` until the next paced
scroll starts. To wait for a scroll to finish:

```sh
until [[ "$(hyprctl vclick status)" == idle* ]]; do sleep 0.02; done
```

## Errors

- `error: bad coordinates`, `error: unknown button`, `error: count must
  be 1-3`, `error: steps must be 1-20`, `error: ms must be 0-1000`: a
  malformed request; the usage follows.
- `error: busy: still scrolling (<done>/<steps>)`: a paced scroll is
  running. Nothing else is taken until it ends.
- `error: busy: …`: the user's mouse is in the same app and holds a
  button, a pointer constraint or a drag there ([design](design.md#the-same-app));
  retry later. Input into other apps goes through meanwhile.
- `error: XWayland windows are not supported`: the compositor's hit test
  asserts on them.
- `error: window not found`, `error: the window is not mapped` (hidden
  or unmapped), `error: <x>,<y> is outside the window (<w>x<h>)` (unless
  one of its popups is there), `error: the window has no surface`,
  `error: the app has no pointer`.
- `error: the session is locked`, `error: the compositor is shutting
  down`.
