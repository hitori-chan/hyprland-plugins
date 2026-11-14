# Testing

## Build

```sh
make -C awesome            # awesome.so: the release plugin (what hyprpm ships)
make -C awesome GATE=1     # awesome-gate.so: the gate's variant, test seams compiled in
make -C awesome test       # the headless unit tests
```

Both need the fork's headers. Test seams are compile-time only
(`#ifdef AWESOME_GATE`). The build stamps its target (compiler, flags,
header roots, the fork's `version.h`), so a new header set rebuilds
everything by itself; a staged set is tracked header by header.

The gate variant compiles through a precompiled header (`core/pch.hpp`,
`-include`d): two thirds the time and ~600 MiB a unit instead of 780, so
the gate's 3 GiB scope builds four at once. The release stays plain, so
a missing include still fails there. The gate's entry points use ccache
when installed (`gate/memcap.sh`); hyprpm's build does not.

## Unit tests

One headless C++26 harness: no compositor runs; it compiles against the
fork's headers and `test/stubs.cpp` supplies their symbols. It covers
the bounded types, box math, the config schema, store and option
admission, wpctl parsing, `Exec=` and launcher quoting, text cutting,
the process runner under the compositor's signal state, the icon-cache
reader, the glyph shapes, the battery's raster and note, and the chat
transcript.

## The gate

The behavioral contract: scenario batteries against a throwaway nested
Hyprland.

```sh
make -C awesome gate ARGS="-s ~/repo/Hyprland [-b quick|all|LIST] [-k LIST] [-p TRACE] [BIN]"
```

- `-s DIR` stages that fork build's headers (checking its `version.h`
  against HEAD and the system's dependency versions), builds the gate
  variant, and runs `DIR/build/Hyprland` (or `BIN`). The `all` tier, or
  `REHEARSE=1`, adds the release variant's deploy rehearsal.
- `-b`: `quick` (default, ~30 s), `all`, or a list of `quick`, `shell`,
  `windows`, `notify`, `system`, `pipeline`, `lifecycle` (run in that
  order); `-k` skips some. `-b windows` for a geometry change, `-b all`
  for a fork bump or a release.
- The summary times the preflight and every battery. `-p TRACE` writes a
  per-line trace; `python3 awesome/gate/profile.py TRACE [N]` ranks
  where the time went.

The result is the summary line, `== stress: ALL N CHECKS PASSED in Ns ==`,
not the exit code. A check missing its precondition is a listed skip,
never a pass; a battery that ran no checks fails. A nested that dies is
caught by its exit status (a signal death fails and keeps the evidence).

New behavior gets a discriminating check, one that fails on the old
code. A check is retired once its code stops changing; dropping one is a
reviewable diff.

### Waiting

Batteries wait on what they check, never on the clock:

- `settle` (one `hyprctl` round trip) is the barrier for anything run
  through a hop: libwayland drains idle callbacks after every dispatch.
- What runs past a hop (a subprocess, a D-Bus reply, a timer, a decode,
  a configure, a warm) is polled: `until_is`/`until_ok` on state,
  `settled CLASS` for a window's box, `shot_until` for the frame that
  draws a change, `hyprctl awesome busy` for the wpctl chains. A poll
  waits for every condition its frame is judged on.
- A fixed wait stays only where nothing is observable: that something
  did not happen.

### Isolation

The nested runs inside a private headless labwc (`gate/host.sh`) with
its own session bus, a private system bus carrying fake logind,
backlight and bluetoothd, scratch XDG dirs, and Aquamarine built at the
fork's `flake.lock` pin with `gate/aquamarine-wayland-swapchain.patch`
(its Wayland output reallocated the swapchain every frame, presenting
undamaged pixels black). The live session receives nothing. The
isolation check fails the gate unless the nested's parent display is the
private host at each launch and, at the end, the live outputs are
unchanged with no nested window there.

- Signature, socket, runtime dir, config, Wayland display and D-Bus
  address all belong to the nested; fixtures always get its
  `WAYLAND_DISPLAY`.
- The fakes shadow `wpctl`, the sound player, logind and BlueZ and never
  touch live devices.
- The harness never removes a Wayland lock file: launch takes the first
  display index with neither a lock nor a socket.

### Rules

- Every entry point runs in its own memory-capped scope
  (`gate/memcap.sh`); a hand-run build or test goes through `systemd-run
  --user --scope -p MemoryMax=1536M -p MemorySwapMax=0`, one at a time. A
  host OOM can kill the live compositor.
- One run at a time: gate, probe, dev loop, launch and stop share a lock
  on `$HYPR_HARNESS`.
- Never rebuild the plugin, edit a gate script or run launch/stop while
  a gate runs: bash reads batteries as it goes, and the nested maps the
  `.so`.
- Run long gates in tmux started in the current login session (an older
  server keeps a stale environment). Scratch goes under
  `/tmp/hypr-gate/`, runtime state under `$HYPR_HARNESS` (default
  `~/.local/share/hypr-nested`).

## Scripts

In `awesome/gate/`, runnable from a clone:

| Script | Does |
|---|---|
| `gate.sh` | the gate (what `make gate` runs) |
| `probe.sh [-b BIN] BODY.sh` | the preflight, then a one-off body with the harness and `lib.sh` sourced; teardown always runs |
| `dev.sh` | one dev-loop iteration: stop, build, launch, open a window, screenshot |
| `launch.sh`, `stop.sh` | start or tear down the host and the nested |
| `shot.sh [out.png]` | screenshot the nested, plus a 2x crop of the bar |
| `sweep.sh BASE TIP [WORKERS]` | build and unit-test every commit in the range against the staged headers; run before a push and after a history rewrite |

Gate-build `hyprctl awesome` verbs:

| Verb | Answer |
|---|---|
| `cells` | the bar's click cells |
| `battery-note` | the battery's note as shown |
| `shown` | the newest card's body as drawn |
| `capsule` | the live capsule's words |
| `center-rows` | the open center's status line and rows |
| `menu` | the open card menu's rows |
| `menu-at LABEL` | where the row whose label starts so sits |
| `dnd-at` | where the center's moon sits |
| `quiet` | the quiet state |
| `busy` | wpctl commands in flight: a battery's barrier |
| `pad NAME` | a virtual pointer stands in for the touchpad; `pad -` drops it |
| `snap-zone` | the armed zone, `<top\|bottom\|-> <left\|right\|->` |

The Wayland fixtures and D-Bus fakes are in
[`devtools/`](../../devtools/README.md).
