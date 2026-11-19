# Testing

## Build

```sh
make -C vclick       # vclick.so against the installed fork's headers (/usr/local)
make -C vclick test  # headless unit cases: hyprutils only
```

An inherited `PKG_CONFIG_PATH` wins (hyprpm's header cache). The build
stamps compiler, flags and the headers' `version.h`; another header set
rebuilds by itself. Compiles go through `$(CCACHE)` when set.

## Unit cases

- **Glides** (thousands planned): start on the start, end exactly on the
  target, stay in the window; Fitts's time for the target's size within
  two sigma of the draw (a correction at most 40 ms more); overshoot and
  bow within bounds (25 %); no tick over 3.5× the first stroke's mean
  step; about `s_share` bend both ways; about `correct_p` of long moves
  correct, short or past, near the first stroke's end and exactly on
  target; a slow end keeps the stroke's speed, adds within 2 s, comes on
  `tail_p` by distance; a drift ends where the stroke starts, never under
  300 px or on a drag; the bow grows by `bow_slope`; a sharper bell peaks
  higher; log-normal draws stay within two sigma.
- **Finger strokes**: Fitts's time within 100–1000 ms, monotonic, slow at
  both ends.
- **Timelines**: every verb's steps, gliding and at once, and their
  bounds.
- **Profile parser**: values, clamps, bad lines skipped, a missing file.
- **Cursor theme reader**: an XCursor file's nearest size, frames and
  delays; bad, truncated and oversized files refused; a shape by an older
  X name, through `Inherits` and `default`; the search path.

## The nested check

```sh
make vclick-check    # or: make -C vclick && bash vclick/check.sh
```

A nested Hyprland (`HYPR_BIN`, default `/usr/local/bin/Hyprland`) with
only vclick loaded, in the gate's private host (`awesome/gate/launch.sh`)
under the gate's lock and memory cap; the live session is never touched.
A `vclick.so` built for another commit's headers is refused before the
launch. Clients: two traced GTK3 windows (`devtools/gtkgrid.py`), A the
target with a custom cursor, B covering A's lower right;
`devtools/vptr` and `devtools/vkbd` are the user's mouse and keyboard.

Each section is judged on the exact protocol events:

- **performance**: clicks land on what A's capture draws, under B and in
  A's popup; double and right clicks; nothing stays, a glide's image
  fades; a drag GTK reads whole within 120 ms, a finger's scroll exact in
  60 ms with its rest, a pinch exact with none.
- **scrolls**: exact steps and detents, the horizontal sign against the
  user's own wheel; the pace (50 ms, `0ms`, `200ms`); `status` and busy
  during a run; stopping when the window closes; waiting out the user's
  button; ending at unload.
- **isolation**: B hears nothing; the user's cursor and focus never move;
  the same-app step-out and back under a moved window; busy under the
  user's held button while B still clicks.
- **keys**: into A with the keyboard in B; into the user's own window
  (the key alone); busy while they hold a key; bad keys and modifiers.
- **human** (the check's profile, no corrections, so time is exactly
  Fitts's): rate, time, rest and landing; hover and drawn image (covered
  by B raised over A); `image`, `where`, `hide`; the user's pointer
  taking the image and the next glide starting where it left; the hover
  ending on a surface change or input into B; a slow glide handing over.
- **held button**: press, a held glide (`dragging`), release against
  GTK's implicit grab; busy while held; a title bar (`devtools/fixwin`)
  asking `xdg_toplevel.move` with the press's serial, the window staying
  put; the user's surface change ending the hold; a GTK drag-and-drop
  refused (source cancelled, no drop on the user's next click);
  `performance`, `hide` and unload letting go.
- **drag**: end checks; human (`moving` then `dragging`, Fitts's time,
  no overshoot, the drop rest); performance; a GTK drag source refused.
- **a hand's click**: hold, double-click gap and bound, `clicking`; a
  popup opened and picked from, also by a right press held down; a title
  bar asking to move at the press stays put; a drag from a grabless popup
  lets go; the lock coming up under the button; the seat ending a hold;
  the user's pointer taking a double click's second click; the window
  closing under the button; unload.
- **finger scroll**: bounds; the exact sum, finger source only, one stop,
  Fitts's time, slow ends, the rest; GTK's smooth scroll and stop; a
  glide there; the user's pointer stopping it.
- **pinch**: bounds; GTK's begin, updates and end, the spread monotonic
  to the exact scale, turns summing exactly, Fitts's time, the rest;
  nothing to B; the user's pointer cancelling it.
- **cursor theme** (the check's, through `XCURSOR_PATH`): B's named
  shapes with their hotspots; a hidden cursor drawn as nothing.
- **lockscreen** (`devtools/vlock`): glide, instant click, paced scroll,
  key and glide click reach A; no cursor; the lock keeps keyboard and
  pointer and alone hears the user; captures work; unlocked, as ever.
- **unload** mid-glide and mid-scroll: the nested stays up; the command
  and Lua functions are gone.
- **requests**: malformed, old words refused, bare addresses, `where`,
  `mode`, `profile`, Lua answers and refusals.

Artifacts: `/tmp/hypr-gate/vclick`. The result is the summary line,
`== vclick: ALL N CHECKS PASSED ==`.
