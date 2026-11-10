# Testing

## Build

```sh
make -C vclick       # vclick.so, against the installed fork's headers (/usr/local)
```

An inherited `PKG_CONFIG_PATH` wins (hyprpm's header cache); the build
stamps its compiler, flags and the headers' `version.h`, so another
header set or an installed fork rebuilds by itself.

## The nested check

```sh
make vclick-check    # or: make -C vclick && bash vclick/check.sh
```

It runs a nested Hyprland in the gate's private host
(`awesome/gate/launch.sh`), under the gate's lock (never alongside a
gate run) and memory cap, with only vclick loaded; the live session is
never touched. Two GTK3 clients (`devtools/gtkgrid.py`, each with a
protocol trace): A is the target, B covers A's lower right and is the
bystander. A virtual pointer (`devtools/vptr`) is the user's mouse.

- Clicks land where A's window capture draws each block, under B and in
  A's popup menu; double and right clicks.
- Scrolls step exactly (GTK's scroll lines and the protocol's detents);
  the horizontal step is held against the user's own wheel through the
  seat, not against GTK's name for it ([design](design.md)).
- The pace: the default steps arrive 50 ms apart, `ms 0` in one burst
  and `ms 200` 200 ms apart (the protocol's own timestamps).
  - `status` reports a run, and any other request is busy meanwhile.
  - A run stops, and says why, when its window closes.
  - A run waits out the user's button in the same app, then finishes.
  - A run ends with an unload, and the nested stays up.
- With the user's pointer on B: B hears nothing; the cursor and the
  focus never move.
- With the user's pointer on A: A's pointer steps out and back in under
  the cursor, after A moved under a still cursor; the user's own click
  still lands.
- With the user's button down in A: `error: busy`, while B still clicks.
- Malformed requests, unload.

Artifacts (captures, logs, traces) stay in `/tmp/hypr-gate/vclick`. The
summary line is `== vclick: ALL N CHECKS PASSED ==`.
