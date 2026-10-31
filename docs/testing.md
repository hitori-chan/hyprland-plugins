# Testing

## Build

```sh
make -C awesome            # awesome.so: the release plugin (what hyprpm ships)
make -C awesome GATE=1     # awesome-gate.so: the gate's variant, test seams compiled in
make -C awesome test       # the headless unit tests
```

Both variants need the fork's headers. Test seams are compile-time only
(`#ifdef AWESOME_GATE`), never an environment switch in the release
binary. The build stamps its target (the compiler and flags, which
carry the header roots, and under a system include dir the fork's
`version.h`), so switching header sets or installing a fork rebuilds
everything by itself; a staged header set is tracked header by header.

The gate variant compiles through a precompiled header of the fork
headers most units include (`core/pch.hpp`, forced in with `-include`):
a unit takes about two thirds of the time and peaks near 600 MiB instead
of 780, so the gate's 3 GiB scope builds four units at once. The release
variant stays plain, so a unit that misses an include still fails there.
The gate's entry points compile through ccache when it is installed
(`gate/memcap.sh` sets the Makefile's `CCACHE` launcher); hyprpm's build
stays the plain compiler.

## Unit tests

One headless C++26 harness. No compositor runs: it compiles against the
fork's headers for the event-loop types, and `test/stubs.cpp` supplies
their symbols. It covers the bounded types, box math, the config schema
(the clamps, the alpha-0 colour guard), store and option admission,
wpctl parsing, `Exec=` and launcher quoting, text cutting, the process
runner under the compositor's signal state, the icon-theme cache reader,
the glyph shapes, the battery's raster and note, a chat card's
transcript and the notch geometry.

## The gate

The plugin's behavioral contract: scenario batteries against a
throwaway nested Hyprland.

```sh
make -C awesome gate ARGS="-s ~/repo/Hyprland [-b quick|all|LIST] [-k LIST] [-p TRACE] [BIN]"
```

- `-s DIR` stages that fork build's headers (and verifies its
  `version.h`), builds the gate variant against them, and runs
  `DIR/build/Hyprland` (a trailing `BIN` names another compositor). The
  release variant's deploy rehearsal runs in the `all` tier (or with
  `REHEARSE=1`).
- `-b` picks the batteries: `quick` (the default, ~30 s smoke), `all`
  (every module battery), or a comma-separated list of `quick`, `shell`,
  `windows`, `notify`, `system`, `pipeline`, `lifecycle` (run in that
  order). `-k` skips some. Run `-b windows` alone for a geometry change
  and `-b all` for a fork bump or a release.
- The summary times the preflight (the builds and the launch) and every
  battery. `-p TRACE` writes a per-line timing trace;
  `python3 awesome/gate/profile.py TRACE [N]` ranks the battery and
  helper lines the time went to.

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

### Isolation

The nested runs inside a private headless labwc (`gate/host.sh`;
install labwc once) with its own session bus, a private system bus
carrying a fake logind, backlight and bluetoothd (a headset's volume
signal), scratch XDG dirs, and Aquamarine built from the
`hitori-chan/aquamarine` checkout's `main`. The live session receives nothing: no output, workspace, window,
focus, cursor, brightness or volume change. The isolation check fails
the gate on a trace: at each launch the nested's parent display must be
the private host, and at the end the live outputs must be unchanged
with no nested window in the live session.

- The signature, socket, runtime dir, config, Wayland display and D-Bus
  address all belong to the nested; fixtures always get the nested's
  `WAYLAND_DISPLAY`.
- The fakes shadow the host's services (`wpctl`, the sound player,
  logind, BlueZ signals) and never touch live devices.
- The harness never removes a Wayland lock file (an empty flock file;
  removing a live one breaks a session): launch takes the first display
  index with neither a lock nor a socket.

### Rules

- Every entry point (gate, probe, dev, launch, sweep) runs in its own
  memory-capped scope (`gate/memcap.sh`). A hand-run build or test goes
  through `systemd-run --user --scope -p MemoryMax=1536M -p
  MemorySwapMax=0`, one at a time: a host OOM can kill the live
  compositor.
- One run at a time: the gate, probe, dev loop, launch and stop share a
  lock on `$HYPR_HARNESS`.
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
| `probe.sh [-b BIN] BODY.sh` | the gate's preflight, then a one-off probe body with the harness and `lib.sh` sourced; teardown always runs |
| `dev.sh` | stop, build, launch, open a window, screenshot: one dev-loop iteration |
| `launch.sh`, `stop.sh` | start or tear down the host and the nested by hand |
| `shot.sh [out.png]` | screenshot the nested, plus a 2x crop of the bar |
| `sweep.sh BASE TIP [WORKERS]` | build and unit-test every commit in the range against the staged headers; run before a push and after a history rewrite |

The gate build adds `hyprctl awesome` verbs for the batteries:

| Verb | Answer |
|---|---|
| `cells` | the bar's click cells |
| `battery-note` | the battery's note as it shows |
| `notch W H` | a W x H notch on every monitor (`notch` alone removes it) |
| `shown` | the newest card's body as drawn |
| `capsule` | the live capsule's words |
| `center-rows` | the open center's status line and rows |
| `menu` | the open card menu's rows |
| `menu-at LABEL` | where a row whose label starts so sits |
| `dnd-at` | where the center's moon sits |
| `quiet` | the quiet state |
| `busy` | the wpctl commands in flight: a battery's barrier |
| `pad NAME` | a virtual pointer stands in for the touchpad; `pad -` drops it |
| `snap-zone` | the armed snap zone, `<top\|bottom\|-> <left\|right\|->` |

The Wayland fixtures and D-Bus fakes the batteries drive are in
[`devtools/`](../devtools/README.md).
