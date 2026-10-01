#!/usr/bin/env bash
# awesome/gate/launch.sh — start the nested Hyprland dev instance and make it
# drivable without ever touching the workspace you're working on. The gate's
# harness calls this (harness.sh); it is also the standalone dev launcher.
#
# How it stays out of your way: the aquamarine WAYLAND backend renders the
# nested desktop into a window of the live session (the headless backend
# needs direct GPU access the tool sandbox denies — it crashes). A hidden
# workspace would starve that window of frames (no screencopy), so instead we
# add an off-screen virtual monitor to the LIVE session (nested-dev) and pin
# the nested window to a dedicated workspace (NEST_WS=99) on it: always
# rendered, always screenshottable, never on your real screen.
#
# Isolation: private dbus session (the nested monolith claims
# org.freedesktop.Notifications and the SNI watcher names on its own bus,
# never the live ones); a minted-fresh instance signature; double-fork +
# setsid so a torn-down caller can't take the instance with it.
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
NEST_WS="${HYPR_NEST_WS:-99}"      # dedicated workspace for the nested window
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
	echo "launch: no live WAYLAND_DISPLAY — run from inside the Wayland session" >&2
	exit 1
fi

hlq() { hyprctl "$@"; }   # live session (inherits the live instance sig)
# the real monitor = the first one that isn't our virtual dev output
real_mon() { hlq monitors -j | python3 -c "import json,sys;print(next(m['name'] for m in json.load(sys.stdin) if m['name']!='$VM'))"; }

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

REAL="$(real_mon)"
# the monitor + workspace you're on right now, so we can hand focus straight
# back after briefly focusing the VM to activate its workspace
FOCUSED_MON="$(hlq monitors -j | python3 -c "import json,sys;print(next((m['name'] for m in json.load(sys.stdin) if m['focused']), '$REAL'))")"
START_WS="$(hlq activeworkspace -j | python3 -c "import json,sys;print(json.load(sys.stdin)['id'])")"

# --- ensure the off-screen virtual monitor exists ---
if ! hlq monitors -j | python3 -c "import json,sys;sys.exit(0 if any(m['name']=='$VM' for m in json.load(sys.stdin)) else 1)"; then
	echo "launch: creating virtual monitor $VM"
	hlq output create headless "$VM" >/dev/null 2>&1
	sleep 0.4
fi
# pin NEST_WS to the VM and make it the VM's ACTIVE (rendered) workspace. Done
# in park() below once the window exists, because activation is only reliable
# after NEST_WS actually lives on the VM.

before_sig="$(ls -1 "$RUNDIR" 2>/dev/null)"
before_aq="$(hlq clients -j 2>/dev/null | python3 -c "import json,sys;print(' '.join(c['address'] for c in json.load(sys.stdin) if c['class']=='aquamarine'))" 2>/dev/null)"

# --- launch, wayland backend, double-fork + setsid detached ---
echo "launch: $BIN -c $CFG (wayland backend -> $VM ws $NEST_WS, private dbus)"
( # unlimited core rlimit: if kernel.core_pattern is a file, a nested SEGV
  # leaves a core for forensics (core_pattern defaults to a pipe that drops)
  ulimit -c unlimited 2>/dev/null
  setsid env -u HYPRLAND_INSTANCE_SIGNATURE AQ_BACKENDS=wayland \
	dbus-run-session -- "$BIN" -c "$CFG" >"$LOG" 2>&1 & )

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
	# Park deterministically (proven sequence). Each step must settle before
	# the next, hence the sleeps; retry the whole thing until the window is
	# verifiably in NEST_WS on the VM.
	parked=0
	for attempt in 1 2 3 4 5; do
		# 1. window -> NEST_WS (silent; creates NEST_WS, initially on $REAL)
		hlq dispatch "hl.dsp.window.move({workspace=\"$NEST_WS\", follow=false, window=\"address:$newaq\"})" >/dev/null 2>&1
		sleep 0.4
		# 2. bind NEST_WS to the VM (does NOT activate it there yet)
		hlq dispatch "hl.dsp.workspace.move({workspace=\"$NEST_WS\", monitor=\"$VM\"})" >/dev/null 2>&1
		sleep 0.4
		# 3. focus the VM then NEST_WS -> now that it lives on the VM this
		#    ACTIVATES it there (= rendered), then focus straight back to the
		#    monitor+workspace you were on. Your $REAL view is unchanged; only
		#    input focus blinks over and back.
		hlq dispatch "hl.dsp.focus({monitor=\"$VM\"})" >/dev/null 2>&1; sleep 0.2
		hlq dispatch "hl.dsp.focus({workspace=\"$NEST_WS\"})" >/dev/null 2>&1; sleep 0.2
		hlq dispatch "hl.dsp.focus({monitor=\"$FOCUSED_MON\"})" >/dev/null 2>&1
		hlq dispatch "hl.dsp.focus({workspace=\"$START_WS\"})" >/dev/null 2>&1
		sleep 0.2
		# 4. return any OTHER workspace the VM grabbed (never NEST_WS itself)
		for w in $(hlq workspaces -j | python3 -c "import json,sys;print(' '.join(str(x['id']) for x in json.load(sys.stdin) if x['monitor']=='$VM' and x['id']!=$NEST_WS))"); do
			hlq dispatch "hl.dsp.workspace.move({workspace=\"$w\", monitor=\"$REAL\"})" >/dev/null 2>&1
		done
		sleep 0.2
		# Verify the WINDOW, not just the workspace: NEST_WS can be the VM's
		# active workspace while the window still sits elsewhere (a move
		# dispatch that silently no-ops parks nothing, and every later capture
		# starves — the message below used to claim success on that state).
		vm_index="$(hlq monitors -j 2>/dev/null | python3 -c "import json,sys;print(next((i for i,m in enumerate(json.load(sys.stdin)) if m['name']=='$VM' and m['activeWorkspace']['id']==$NEST_WS),''))" 2>/dev/null)"
		if [[ -n "$vm_index" ]] && hlq clients -j 2>/dev/null | python3 -c "import json,sys;sys.exit(0 if any(c['class']=='aquamarine' and c['address']=='$newaq' and c['workspace']['id']==$NEST_WS and c['monitor']==int(sys.argv[1]) for c in json.load(sys.stdin)) else 1)" "$vm_index" 2>/dev/null; then
			parked=1
			break
		fi
	done
	if [[ "$parked" == "1" ]]; then
		echo "launch: parked nested window $newaq on $VM ws $NEST_WS (attempt $attempt)"
	else
		echo "launch: WARN park unverified after 5 attempts (window $newaq not confirmed on $VM ws $NEST_WS); continuing — the warmup detects a dead frame cycle" >&2
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
