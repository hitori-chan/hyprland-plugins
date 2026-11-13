# Testing

## Build

```sh
make -C vclick       # vclick.so, against the installed fork's headers (/usr/local)
```

An inherited `PKG_CONFIG_PATH` wins (hyprpm's header cache). The build
stamps its compiler, flags and the headers' `version.h`, so another
header set rebuilds by itself.

## Unit cases

```sh
make -C vclick test  # headless: path.cpp with only hyprutils, theme.cpp alone
```

Thousands of planned glides, each held to the design: it starts on its
start and ends exactly on its target, stays in the window, takes Fitts's
time for the target's size (a correction adds at most 300 ms),
overshoots and bows within bounds (25 %), no tick moves over 3× the
average step, and about `overshoot_p` of long moves overshoot. A
finger's stroke: Fitts's time within 100–1000 ms, from none to all of
the way, never back, slow at both ends. Plus the profile parser: values, clamps, skipped bad lines, a missing file. The
cursor theme reader: an XCursor file's nearest size, its frames and
delays, refused bad, truncated and oversized files; a shape found by an
older X name, through `Inherits` and `default`; the search path.

## The nested check

```sh
make vclick-check    # or: make -C vclick && bash vclick/check.sh
```

A nested Hyprland (`HYPR_BIN`, default `/usr/local/bin/Hyprland`) in
the gate's private host (`awesome/gate/launch.sh`), under the gate's
lock and memory cap (never alongside a gate run), with only vclick
loaded; the live session is never touched. A `vclick.so` built for
another commit's headers is refused before the launch. Two GTK3 clients
(`devtools/gtkgrid.py`, each traced): A is the target with a custom
cursor image, B covers A's lower right as the bystander. `devtools/vptr`
and `devtools/vkbd` are the user's mouse and keyboard.

It covers, each against the exact protocol events:

- **tap mode**: clicks land on what A's window capture draws, under B
  and in A's popup; double and right clicks.
- **scrolls**: exact steps and detents, the horizontal sign matched
  against the user's own wheel; the pace (50 ms default, `0ms` burst,
  `200ms`), `status` and busy during a run, a run stopping when its
  window closes, waiting out the user's button, ending at unload.
- **isolation**: B hears nothing, the user's cursor and focus never move;
  the same-app step-out and back under a moved window; `error: busy`
  under the user's held button while B still clicks.
- **keys**: into A with the keyboard in B (enter, modifiers, press,
  release, leave), into the user's own window (the key alone), busy
  while they hold a key, bad keys and modifiers.
- **mouse mode** (the check's own profile, no overshoot, so the time is
  Fitts's exactly): glide rate, time, dwell and landing; the held hover
  and the drawn image (covered by B when raised over A); `image`;
  `where`; `hide`; the user's pointer taking the image and the next
  glide starting where it left; hover ending on a surface change or an
  input into B; a slow glide handing over to a jump.
- **tap mode glide**: nothing stays; the image fades after landing.
- **a held button**: press, a held glide (`dragging`), release, against
  the protocol events and GTK's implicit grab (up in the press's block);
  busy while held; a title bar (`devtools/fixwin` with `drag`) asking
  `xdg_toplevel.move` with the press's serial and the window staying put
  while the user's mouse moves; the user's pointer changing surface
  ending the hold; a GTK drag-and-drop refused (its source cancelled,
  the hold ended, the user's next click offering no drop); tap mode;
  `hide` letting go; unload letting go.
- **drag**: the end's checks; a drag in mouse mode (`moving`, then
  `dragging`; Fitts's time, no overshoot, the dwell before the release;
  GTK's press and release in their blocks); a jump drag in tap mode; a
  drag from a GTK drag source refused.
- **a finger's scroll**: its bounds; a stroke summing to the request
  exactly, every frame from a finger, no wheel steps, one stop, Fitts's
  time, slow ends, the rest before the lift; GTK reading a touchpad's
  smooth scroll and its stop (its blue block); a glide there; the user's
  pointer stopping it; the tap mode's leave.
- **pinch**: its bounds; GTK's two-finger begin, updates and end, the
  spread only growing and ending exactly on the scale, the turns summing
  to the request, Fitts's time, the rest; nothing to B; a glide there
  and a pinch out; the user's pointer cancelling it.
- **the cursor theme** (the check's own, through `XCURSOR_PATH`): B's
  named shapes drawn from it with their hotspots, its hidden cursor
  drawn as nothing.
- **lockscreen** (`devtools/vlock`): a glide, jump click, paced scroll,
  key and glide click reach A; no cursor drawn; the lock surface keeps
  the keyboard and pointer, and the user's own input goes only to it;
  window captures work. Unlocked, input goes as ever.
- **unload** during a glide and a paced scroll; the nested stays up and
  the command and Lua functions are gone.
- **requests**: malformed ones, bare addresses, `where`, `mode`,
  `profile`, and the Lua functions' answers and refusals.

Artifacts stay in `/tmp/hypr-gate/vclick`. The summary line is
`== vclick: ALL N CHECKS PASSED ==`.
