# awesome/gate/live.sh — identify the LIVE compositor, read-only. Sourced
# by launch.sh, stop.sh and harness.sh.
#
# The harness never acts on the live session (the nested runs inside a
# private host, host.sh). It only needs to know which Hyprland process is
# the user's, so orphan sweeps can never kill it, and to read its state for
# the isolation check. The live instance is the session's compositor: its
# lock names a running process the session started (start-hyprland passes
# --watchdog-fd; a nested is started with -c and never has it) that answers
# on its control socket. Never trust the caller's
# HYPRLAND_INSTANCE_SIGNATURE (a live relog leaves it naming the dead
# session) or a bare socket file (a dead instance dir may sort first).
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
LIVE_SIG="" LIVE_PID=""

live_resolve() {
	local pass s sig pid
	# pass 1: the session-started compositor; pass 2 (no watchdog launcher):
	# any responsive instance not started with a -c config
	for pass in watchdog plain; do
		for s in "$RUNDIR"/*/; do
			sig="$(basename "$s")"
			[[ -S "$s/.socket.sock" ]] || continue
			pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
			[[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null || continue
			if [[ $pass == watchdog ]]; then
				grep -Fzxq -- "--watchdog-fd" "/proc/$pid/cmdline" 2>/dev/null || continue
			else
				grep -Fzxq -- "-c" "/proc/$pid/cmdline" 2>/dev/null && continue
			fi
			timeout 2 hyprctl -i "$sig" version >/dev/null 2>&1 || continue
			LIVE_SIG="$sig" LIVE_PID="$pid"
			return 0
		done
	done
	return 1
}
live_resolve

# read-only queries of the live session (the isolation check): never a
# dispatch, keyword, output or reload command
live_read() {
	[[ -n "$LIVE_SIG" ]] || return 1
	case "$1" in
		monitors | clients | workspaces | activeworkspace | cursorpos) timeout 3 hyprctl -i "$LIVE_SIG" "$@" ;;
		*) echo "live_read: refusing '$1' (read-only queries only)" >&2; return 1 ;;
	esac
}

# A harness-style nested: a Hyprland that is not the live one, started
# with a -c config and without the session's --watchdog-fd. Orphan sweeps
# match this, so a nested launched from any (old, /tmp debug) config is
# still reaped — and the live PID never is.
is_nested_pid() {
	[[ -n "$1" && "$1" != "$LIVE_PID" ]] || return 1
	grep -Fzxq -- "-c" "/proc/$1/cmdline" 2>/dev/null && ! grep -Fzxq -- "--watchdog-fd" "/proc/$1/cmdline" 2>/dev/null
}

# The nested's real log: the instance file (stdout carries only the lines
# before the config switches stdout logging off). Hyprland buffers it; the
# rolling log (hyprctl rollinglog) is the live view.
nested_log() { printf '%s\n' "$RUNDIR/${1:-${SIG:-}}/hyprland.log"; }

# Remove the runtime dirs of DEAD nested instances, keeping the newest
# one's log as $HARNESS/nested.log (the previous one as .prev). A nested is
# recognized by its own log ("launched without start-hyprland": the session
# compositor never is); dead dirs of past LIVE sessions are left alone —
# they may be crash evidence.
sweep_nested_dirs() {
	local s pid newest="" keep="${HARNESS:-${HARNESS_DIR:-}}" dead=()
	for s in $(ls -dt "$RUNDIR"/*/ 2>/dev/null); do
		# a clean exit removes the lock; a lock names a pid that may live
		pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
		[[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null && continue
		grep -q 'launched without start-hyprland' "$s/hyprland.log" 2>/dev/null || continue
		[[ -z "$newest" ]] && newest="$s"
		dead+=("$s")
	done
	if [[ -n "$newest" && -n "$keep" && -s "$newest/hyprland.log" ]]; then
		[[ -f "$keep/nested.log" ]] && mv -f "$keep/nested.log" "$keep/nested.log.prev"
		cp -f "$newest/hyprland.log" "$keep/nested.log" 2>/dev/null
	fi
	((${#dead[@]})) && rm -rf -- "${dead[@]}"
	return 0
}
