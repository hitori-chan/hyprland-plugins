#!/usr/bin/env bash
# awesome/gate/launch.sh — start the nested Hyprland dev instance and make it
# drivable without ever touching the workspace you're working on. The gate's
# harness calls this (harness.sh); it is also the standalone dev launcher.
#
# How it stays out of your way: the aquamarine WAYLAND backend renders the
# nested desktop into a window of the LIVE session (the headless backend
# needs direct GPU access the tool sandbox denies — it crashes). A hidden
# workspace would starve that window of frames (no screencopy), so instead we
# add an off-screen virtual monitor to the LIVE session (nested-dev). The
# nested is spawned through the live compositor's exec with the exec-rule
# {monitor=nested-dev silent}: its root toplevel maps STRAIGHT onto the
# VM's active workspace without taking focus (the monitor rule's "silent"
# suffix; CWindow::mapWindow). That is the whole park — no moves, no
# workspace churn, zero live-focus dispatches (user rule 2026-10-02).
# Two hard constraints behind the shape:
#   * a window on a NON-active workspace is never painted by the host, so
#     the frame callbacks stop and the nested's render loop starves
#     (captures hang, S1 hangs) — the window must sit on the VM's ACTIVE ws;
#   * moving a workspace that holds the focused window (hl.workspace.move,
#     carryFocus default) fires rawMonitorFocus(VM) + a live cursor warp —
#     the old map-to-REAL + window.move + workspace.move park did exactly
#     that on every relaunch (the live-focus canary caught it, 2026-10-02).
#
# Isolation: private dbus session (the nested monolith claims
# org.freedesktop.Notifications and the SNI watcher names on its own bus,
# never the live ones); a minted-fresh instance signature; setsid so a
# torn-down caller can't take the instance with it.
#
# Runtime state (nested.sig / nested.wl / nested.log) goes to $HARNESS,
# default ~/.local/share/hypr-nested — keep the repo tree free of state.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
mkdir -p "$HARNESS_DIR"
CFG="${HYPR_CFG:-$HERE/nested.lua}"
BIN="${HYPR_BIN:-/usr/local/bin/Hyprland}"
LOG="$HARNESS_DIR/nested.log"
VM="nested-dev"                    # the off-screen virtual monitor
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
	echo "launch: no live WAYLAND_DISPLAY — run from inside the Wayland session" >&2
	exit 1
fi

hlq() { hyprctl "$@"; }   # live session (inherits the live instance sig)

# --- kill a prior nested instance from this config (never the live one) ---
KILLED=""
for s in "$RUNDIR"/*/; do
	sig="$(basename "$s")"
	[[ "$sig" == "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] && continue
	pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
	[[ -n "$pid" ]] || continue
	if grep -qa -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null; then
		echo "launch: killing prior nested $sig (pid $pid)"
		kill "$pid" 2>/dev/null
		KILLED="$KILLED $pid"
	fi
done
# Wait for the killed instances to fully exit before spawning the next one.
# Launching a new nested compositor while a sibling's client teardown is
# still running on the host can race backend init against host-side buffer
# release and die during dmabuf enumeration (seen aq 0.14 host vs 0.15
# nested). A fixed 0.4s sleep is not reliable under load.
for k in $KILLED; do
	for _ in $(seq 1 50); do
		kill -0 "$k" 2>/dev/null || break
		sleep 0.1
	done
	if kill -0 "$k" 2>/dev/null; then
		echo "launch: warning: prior nested $k still alive after 5s, SIGKILL"
		kill -9 "$k" 2>/dev/null
		sleep 0.5
	fi
done
sleep 0.3

# --- ensure the off-screen virtual monitor exists ---
if ! hlq monitors -j | python3 -c "import json,sys;sys.exit(0 if any(m['name']=='$VM' for m in json.load(sys.stdin)) else 1)"; then
	echo "launch: creating virtual monitor $VM"
	hlq output create headless "$VM" >/dev/null 2>&1
	sleep 0.4
fi
# NO live focus dispatches anywhere in this script (user rule 2026-10-02):
# the exec-rule park maps the window silently onto the VM's active ws, and
# the VM keeps rendering it (damage on its surface schedules frames on the
# overlapping output; IHyprRenderer::damageSurface), so the harness capture
# works without any focus or cursor touch. Pin the dev output's
# mode+scale to the panel's, every join. A
# DIVERGENT-mode output reconfigures the dmabuf feedback table on
# join/leave, and one rapid divergent-mode remove+create killed the
# live compositor on this i915 box (2026-10-02, hyprpaper SEGV). The
# pin is machine-specific, so it lives here, not in the user config,
# and re-applies to a surviving output.
PIN="$(hlq monitors -j 2>/dev/null | python3 -c "
import json,sys
ms=json.load(sys.stdin)
real=next((m for m in ms if m['name']!='$VM'),None)
vm=next((m for m in ms if m['name']=='$VM'),None)
key=lambda m:(m['width'],m['height'],m['refreshRate'],m['scale'])
if real and vm and key(real)!=key(vm):
    print('hl.monitor({output=\"$VM\", mode=\"%dx%d@%.5f\", scale=%g})'%key(real))
" 2>/dev/null)"
if [[ -n "$PIN" ]]; then
	echo "launch: pinning $VM to the panel's mode/scale: $PIN"
	hlq dispatch "(function() $PIN return hl.dsp.no_op() end)()" >/dev/null 2>&1
	sleep 0.5
	AFTER="$(hlq monitors -j 2>/dev/null | python3 -c "
import json,sys
ms=json.load(sys.stdin)
real=next((m for m in ms if m['name']!='$VM'),None)
vm=next((m for m in ms if m['name']=='$VM'),None)
print('%dx%d@%.3f/%g'%(vm['width'],vm['height'],vm['refreshRate'],vm['scale']) if vm else 'absent')
" 2>/dev/null)"
	echo "launch: $VM now ${AFTER:-unreadable}"
fi
before_sig="$(ls -1 "$RUNDIR" 2>/dev/null)"
before_aq="$(hlq clients -j 2>/dev/null | python3 -c "import json,sys;print(' '.join(c['address'] for c in json.load(sys.stdin) if c['class']=='aquamarine'))" 2>/dev/null)"

# --- launch via the live compositor's exec, with the exec-rule -------------
# The executor (CExecutor::spawnRawProc) runs `sh -c '<cmd>'` FORKED FROM
# THE LIVE COMPOSITOR — so the nested inherits the live compositor's
# environment, NOT this script's. The harness's stress env (XDG_STATE_HOME/
# XDG_CACHE_HOME to the scratch state, the fake-wpctl PATH + AW_* files) is
# therefore INLINED into the exec string; the first version of this launch
# inherited the live env and the gate wrote the battery's stores into the
# user's real ~/.local/state (2026-10-02). Standalone (no harness), the
# values are the caller session's own — the pre-exec_cmd inheritance
# behavior.
# A single `exec` chain keeps the exec'd process's PID: sh execs (no fork
# for a simple command), setsid does not fork when it is not a group
# leader, env execs, and dbus-run-session execs the target after forking
# its private dbus — so the nested Hyprland's client PID is the one the
# exec rule registered, and its root toplevel picks the rule up at map
# time. The 60s exec-rule expiry (IRule::markAsExecRule) covers boot.
echo "launch: $BIN -c $CFG (wayland backend -> $VM, private dbus)"
exec_env="AQ_BACKENDS=wayland"
for v in XDG_STATE_HOME XDG_CACHE_HOME PATH AW_WPCTL_LOG AW_WPCTL_HANG_FILE AW_WPCTL_FLOOD_FILE AW_SOUND_HANG_FILE; do
	[[ -n "${!v:-}" ]] && exec_env="$exec_env $v=${!v}"
done
exec_rc=0
hlq dispatch "hl.dsp.exec_cmd('ulimit -c unlimited 2>/dev/null; exec setsid env -u HYPRLAND_INSTANCE_SIGNATURE $exec_env dbus-run-session -- $BIN -c $CFG > $LOG 2>&1', {monitor='$VM silent'})" >/dev/null 2>&1 || exec_rc=$?
if [[ $exec_rc -ne 0 ]]; then
	echo "launch: hl.dsp.exec_cmd failed (rc=$exec_rc) — the exec-rule park needs a fork with the monitor-rule silent suffix; refusing to fall back to the focus-dragging moves" >&2
	exit 1
fi

# --- wait for the new instance signature ---
sig=""
for _ in $(seq 1 100); do
	sleep 0.1
	for s in "$RUNDIR"/*/; do
		cand="$(basename "$s")"
		if ! grep -qx "$cand" <<<"$before_sig" && [[ -S "$s/.socket.sock" ]]; then sig="$cand"; break 2; fi
	done
done
if [[ -z "$sig" ]]; then
	echo "launch: timed out waiting for nested instance — see $LOG" >&2
	grep -ivE "xkbcomp|Warning:|^>|DEBUG" "$LOG" | tail -15 >&2
	exit 1
fi
# the client Wayland socket lands on line 2 of the lock a beat after the sig
# dir appears — poll for it rather than racing
wl=""
for _ in $(seq 1 30); do
	wl="$(sed -n 2p "$RUNDIR/$sig/hyprland.lock" 2>/dev/null)"
	[[ -n "$wl" ]] && break
	sleep 0.1
done

# --- park the nested window on the virtual monitor ---
newaq=""
for _ in $(seq 1 40); do
	newaq="$(hlq clients -j 2>/dev/null | python3 -c "
import json,sys
before=set('''$before_aq'''.split())
[print(c['address']) or sys.exit() for c in json.load(sys.stdin) if c['class']=='aquamarine' and c['address'] not in before]" 2>/dev/null)"
	[[ -n "$newaq" ]] && break
	sleep 0.1
done
if [[ -n "$newaq" ]]; then
	# The exec-rule did the park at map time: the window must already sit on
	# the VM's ACTIVE workspace. Verify, don't fix — there is no focus-safe
	# fix (moving it would be the dance we removed), so a miss is a hard
	# fail: off the VM's active ws the window is unpainted and the nested's
	# frame cycle starves (captures hang, S1 hangs).
	parked=0
	for attempt in 1 2 3 4 5; do
		# ws ids are unique across monitors, so "window's ws == the VM's
		# active ws" proves placement AND paintability in one check.
		vm_active="$(hlq monitors -j 2>/dev/null | python3 -c "import json,sys;print(next((m['activeWorkspace']['id'] for m in json.load(sys.stdin) if m['name']=='$VM'),''))" 2>/dev/null)"
		if [[ -n "$vm_active" ]] && hlq clients -j 2>/dev/null | python3 -c "
import json,sys
cs = json.load(sys.stdin)
c = next((c for c in cs if c['class']=='aquamarine' and c['address']=='$newaq'), None)
sys.exit(0 if c and c['workspace']['id'] == int(sys.argv[1]) else 1)" "$vm_active" 2>/dev/null; then
			parked=1
			break
		fi
		sleep 0.5
	done
	if [[ "$parked" == "1" ]]; then
		echo "launch: parked nested window $newaq on the $VM active ws (attempt $attempt)"
	else
		echo "launch: FAIL nested window $newaq did not land on the $VM active ws via the exec-rule park — off it the window is unpainted and every capture would hang" >&2
		exit 1
	fi
else
	echo "launch: WARN could not find nested window to park" >&2
fi

# --- pin the window to the config's extent --------------------------------
# The live compositor maps the aquamarine window FLOATING at its own default
# size, and the nested output follows the window: the gate would otherwise run
# on a too-small viewport where tall states (a full 7-line transcript row)
# exceed the shade's body cap and the row drops out. Resize the window to the
# mode the config asked for; the nested monitor converges.
if [[ -n "$newaq" ]]; then
	 nest_extent="$(sed -n 's/.*mode[[:space:]]*=[[:space:]]*"\([0-9]\+x[0-9]\+\)@.*/\1/p' "$CFG" | head -1)"
	 if [[ "$nest_extent" == *x* ]]; then
	 	hlq dispatch "hl.dsp.window.resize({x=${nest_extent%x*}, y=${nest_extent#*x}, window=\"address:$newaq\"})" >/dev/null 2>&1
	 	sleep 0.5
	 fi
fi

printf '%s\n' "$sig" >"$HARNESS_DIR/nested.sig"
printf '%s\n' "$wl" >"$HARNESS_DIR/nested.wl"
echo "launch: nested up"
echo "  SIG=$sig  WL=$wl"
echo "  hyprctl -i $sig <cmd>   |   $(dirname "$0")/shot.sh   |   log: $LOG"
