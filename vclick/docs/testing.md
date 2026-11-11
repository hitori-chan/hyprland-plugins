# Testing

## Build

```sh
make -C vclick       # vclick.so, against the installed fork's headers (/usr/local)
```

An inherited `PKG_CONFIG_PATH` wins (hyprpm's header cache); the build
stamps its compiler, flags and the headers' `version.h`, so another
header set or an installed fork rebuilds by itself.

## The path's unit cases

```sh
make -C vclick test  # headless: path.cpp with no compositor, only hyprutils
```

Thousands of planned glides, each held to the design: it starts on its
start and ends exactly on its target, stays in the window, takes
Fitts's time (a correction adds at most 300 ms), never runs past the
target by more than the overshoot bound or bows more than 25%, no tick
moves over 3× the average step, and about `overshoot_p` of the long ones
overshoot. The profile parser: its values, a clamped value, a
skipped bad one, a missing file.

## The nested check

```sh
make vclick-check    # or: make -C vclick && bash vclick/check.sh
```

It runs a nested Hyprland in the gate's private host
(`awesome/gate/launch.sh`), under the gate's lock (never alongside a
gate run) and memory cap, with only vclick loaded; the live session is
never touched. Two GTK3 clients (`devtools/gtkgrid.py`, each with a
protocol trace): A is the target, B covers A's lower right and is the
bystander; A sets a custom cursor image (`gtkgrid.py … cursor`). A
virtual pointer (`devtools/vptr`) is the user's mouse.

- In the tap mode (its exact event sequences): clicks land where A's
  window capture draws each block, under B and in A's popup menu;
  double and right clicks.
- Scrolls step exactly (GTK's scroll lines and the protocol's detents);
  the horizontal step is held against the user's own wheel through the
  seat, not against GTK's name for it ([design](design.md)).
- The pace: the default steps arrive 50 ms apart, `0ms` in one burst
  and `200ms` 200 ms apart (the protocol's own timestamps).
  - `status` reports a run, and any other request (`hide` too) is busy
    meanwhile.
  - A run stops, and says why, when its window closes.
  - A run waits out the user's button in the same app, then finishes.
  - A run ends with an unload, and the nested stays up.
- With the user's pointer on B: B hears nothing; the cursor and the
  focus never move.
- With the user's pointer on A: A's pointer steps out and back in under
  the cursor, after A moved under a still cursor; the user's own click
  still lands.
- With the user's button down in A: `error: busy`, while B still clicks.
- The mouse mode, with a profile file of the check's own (never the
  user's) and no overshoot, so a glide's time is Fitts's law exactly:
  - a click glides from the held hover: motions at the profile's rate,
    Fitts's time, a dwell, then the click and no leave, the last motion
    exactly on target; `status` says `moving`, and anything else is busy
    meanwhile;
  - B hears nothing; the cursor and the focus never move;
  - `image` names A's own image (`set_cursor`) and B's named shape;
  - a move goes from where the click left A's pointer (motions only),
    `where` says where, A's own image is drawn there in a screen
    capture, and is still there 2 s later; a click there comes from the
    hover and keeps it;
  - `hide`: A's pointer leaves, `where` says `nowhere`, the image is
    gone; a jump click enters, clicks, stays, and draws the image there;
  - the user's pointer in A hides the image (and `where` says so); out
    again, it is drawn again;
  - the user's pointer changing surface ends the hover, and so does a
    click into B, where the mouse then is;
  - the user's pointer comes into A during a (slow) glide: it hands over
    to a jump, which still hits, and `status` says so.
- The tap mode's glide: nothing stays at the switch; the click enters,
  clicks and leaves; the image is drawn as it lands, then fades.
- Unload during a glide (it ends, A's pointer leaves) and during a paced
  scroll; the nested stays up.
- Malformed requests (an unknown or repeated option, a scroll without a
  direction, the bounds), a bare `0x…` address, `where`, `mode`,
  `profile`.

Artifacts (captures, logs, traces) stay in `/tmp/hypr-gate/vclick`. The
summary line is `== vclick: ALL N CHECKS PASSED ==`.
