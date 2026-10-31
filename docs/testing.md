# Testing

## Build

```sh
make -C awesome            # awesome.so: the release plugin (what hyprpm ships)
make -C awesome GATE=1     # awesome-gate.so: the gate's variant, test seams compiled in
make -C awesome test       # the headless unit tests
```

Both variants need the fork's headers. Test seams are compile-time only
(`#ifdef AWESOME_GATE`), never an environment switch in the release
binary. The build stamps its target (compiler, flags, the headers'
`version.h`), so switching header sets rebuilds everything by itself.

The gate's entry points compile through ccache when it is installed
(`gate/memcap.sh` sets the Makefile's `CCACHE` launcher), and sweep
worktrees share the hits. hyprpm's build stays the plain compiler.

The gate variant takes a precompiled header of the fork headers most
units include (`core/pch.hpp`, forced in with `-include`): a unit
compiles in about two thirds of the time and peaks near 600 MiB instead
of 780, so the gate's 3 GiB scope builds four units at once instead of
three (a cold gate build: 260 s before, 114 s with it). The release
variant stays plain, so a unit that misses an include still fails there.

## Unit tests

One headless C++26 harness. No compositor runs: it compiles against the
fork's headers for the event-loop types, and `test/stubs.cpp` supplies
their symbols. It covers the bounded types, box math, the config schema
(the clamps, the alpha-0 colour guard), store and option admission,
wpctl parsing, `Exec=` and launcher quoting, text cutting, the process
runner under the compositor's signal state, the icon-theme cache reader
and the glyph shapes.

## The gate

The plugin's behavioral contract: scenario batteries against a
throwaway nested Hyprland.

```sh
make -C awesome gate ARGS="-s ~/repo/Hyprland [-b quick|all|LIST] [-k LIST]"
```

- `-s DIR` stages that fork build's headers (and verifies its
  `version.h`), builds the gate variant against them, and runs
  `DIR/build/Hyprland`. The release variant's deploy rehearsal runs in
  the `all` tier (or with `REHEARSE=1`); the sweep builds every commit's
  release before a push.
- `-b` picks the batteries: `quick` (the default, ~30 s smoke), `all`
  (~3.5 min of batteries), or a list of `shell`, `windows`, `notify`,
  `system`, `pipeline`, `lifecycle` (run in that order). `-k` skips some.
- Run `-b windows` alone for a geometry change and `-b all` for a fork
  bump or a release.
- The summary times the preflight (the builds and the launch) and every
  battery; `-p TRACE` with `gate/profile.py` names the lines the time
  went to.

The result is the summary line, `== stress: ALL N CHECKS PASSED in Ns ==`,
not the exit code. A check whose precondition is missing is a skip,
listed under the summary and never counted as a pass; a battery that ran
no checks fails. A nested that dies is caught by its exit status (a
signal death fails the gate and keeps the evidence), not by log greps.

New behavior gets a discriminating check, one that fails against the old
code. A check is retired once its code stops changing; dropping one is a
reviewable diff.

### Waiting

A battery waits on what it checks, never on the clock:

- `settle` (one `hyprctl` round trip) is the barrier for anything that
  runs through a hop: libwayland drains the compositor's idle callbacks
  after every dispatch.
- What runs past a hop (a subprocess, a D-Bus reply, a timer, an image
  decode, a client's configure, a texture's warm-up) is polled:
  `until_is`/`until_ok` on state, `settled CLASS` for a window's box,
  `shot_until` for the frame that draws a change, `hyprctl awesome busy`
  for the wpctl chains. A poll waits for every condition its frame is
  judged on, so a check of what must not draw reads the new frame.
- A fixed wait stays only where nothing is observable: that something
  did not happen after an asynchronous trigger.

`-p TRACE` writes a per-line timing trace of the batteries;
`python3 awesome/gate/profile.py TRACE [N]` ranks the battery lines and
the helper lines the time went to.

### Isolation

The nested runs inside a private headless labwc (`gate/host.sh`;
install labwc once) with its own session bus, a private system bus
carrying a fake logind, backlight and bluetoothd (a headset's volume
signal), scratch
XDG dirs, and aquamarine at the fork's `flake.lock` pin. The live session
receives nothing: no output, workspace, window, focus, cursor,
brightness or volume change. The isolation check (structural at each
launch, observed at the end) fails the gate on any trace.

- The signature, socket, runtime dir, config, Wayland display and D-Bus
  address all belong to the nested; fixtures always get the nested's
  `WAYLAND_DISPLAY`.
- The fakes shadow the host's services (`wpctl`, the sound player,
  logind, BlueZ signals) and never touch live devices.
- A "dead" Wayland display is verified with `flock`, never by its lock
  file: those are empty flock files, and removing a live one breaks a
  session.

### Rules

- Every entry point (gate, probe, dev, launch, sweep) runs in its own
  memory-capped scope (`gate/memcap.sh`). A hand-run build or test goes
  through `systemd-run --user --scope -p MemoryMax=1536M -p
  MemorySwapMax=0`, one at a time: a host OOM can kill the live
  compositor.
- One run at a time: the gate, probe and dev loop share a lock on
  `$HYPR_HARNESS`.
- Never rebuild the plugin, edit a gate script or run launch/stop while
  a gate runs: bash reads battery scripts as it goes, and the nested maps
  the `.so`.
- Run long gates in tmux (a server started in the current login
  session: one that outlived its session keeps a stale environment).
  Scratch goes under `/tmp/hypr-gate/`, runtime state under
  `$HYPR_HARNESS` (default `~/.local/share/hypr-nested`).

## Scripts

All in `awesome/gate/`, runnable from a clone as-is:

| Script | Does |
|---|---|
| `gate.sh` | the gate (what `make gate` runs) |
| `probe.sh -b BIN BODY.sh` | the gate's preflight, then a one-off probe body with the harness and `lib.sh` sourced; teardown always runs |
| `dev.sh` | stop, build, launch, open a window, screenshot: one dev-loop iteration |
| `launch.sh`, `stop.sh` | start or tear down the host and the nested by hand |
| `shot.sh [out.png]` | screenshot the nested, plus a 2x crop of the bar |
| `sweep.sh BASE TIP` | build and unit-test every commit in the range against the staged headers; run before a push and after a history rewrite |

The gate build adds `hyprctl awesome` verbs for the batteries: `cells`
(the bar's click cells), `battery-note` (the battery's note showing),
`shown` (the newest card's body as drawn), `center-rows` (the open
center's status line and rows), `menu` (the open menu's rows),
`menu-at LABEL` (where a row whose label starts so sits), `dnd-at` (where
the center's moon sits), `quiet` (the quiet state), `busy` (the wpctl commands in
flight: a battery's barrier), `pad NAME` (a virtual pointer stands in
for the touchpad; `pad -` drops it) and `snap-zone` (the armed snap
zone, "<top|bottom|-> <left|right|->").

The Wayland fixtures and D-Bus fakes the batteries drive are in
[`devtools/`](../devtools/README.md).
