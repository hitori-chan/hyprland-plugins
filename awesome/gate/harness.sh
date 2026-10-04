# SECONDS-based wall clock, in s since gate.sh started: each line shows
# where the minutes go, so a slow battery is visible without a profiler.
ok()  { PASS=$((PASS + 1)); printf '  ok  [%s s] %s\n' "$SECONDS" "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL [%s s] %s\n' "$SECONDS" "$1"; }
chk() { # chk <name> <command...> — command's exit code decides
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
}

# --- live session access ----------------------------------------------------
# The LIVE instance = the instance dir holding a live control socket. Never
# trust the caller's HYPRLAND_INSTANCE_SIGNATURE: after a live relog it
# points at the dead session and every default-socket hyprctl fails (rc=4)
# (2026-10-04: launch broke for a day after the user relogged). Every LIVE
# operation in the harness goes through hlq with the resolved sig.
LIVE_SIG=""
if [[ -n "${RUNDIR:-}" ]]; then
	for _s in "$RUNDIR"/*/; do
		[[ -S "$_s/.socket.sock" ]] && { LIVE_SIG="$(basename "$_s")"; break; }
	done
	unset _s
fi
export HYPRLAND_INSTANCE_SIGNATURE="${LIVE_SIG:-${HYPRLAND_INSTANCE_SIGNATURE:-}}"
hlq() { [[ -n "$LIVE_SIG" ]] && hyprctl -i "$LIVE_SIG" "$@"; }

# --- live session canary ----------------------------------------------------
# User rule (2026-10-02, absolute): tests/gates never affect the live
# workspace and never take the user's mouse or focus. The harness samples
# the LIVE focused monitor at 10Hz for the whole run and fails the gate if
# it ever lands on the gate's off-screen output (nested-dev) — a state only
# the gate itself can produce (the user cannot focus an off-screen monitor).
# This is the regression net for the parking focus dance, which focused
# nested-dev on every launch/warmup/stop and stole the user's keyboard
# focus (the dance is gone from launch.sh/harness.sh; this proves it).
# The cursor position is NOT sampled: the user moves the mouse during a
# run (false positives), and the mouse yank lived in the focus dance, which
# the focused-monitor watch above does catch.
CANARY_PID="" CANARY_FILE=""
live_canary_start() {
	[[ -n "$CANARY_PID" ]] && return
	CANARY_FILE="$HARNESS/live-canary.log" # $HARNESS: $STATE is wiped by cleanup
	: >"$CANARY_FILE"
	( while :; do
			mon="$(hlq monitors -j 2>/dev/null | python3 -c "import json,sys;print(next((m['name'] for m in json.load(sys.stdin) if m.get('focused')), ''))" 2>/dev/null)"
			[[ "$mon" == "nested-dev" ]] && printf '%s\n' "$(date +%H:%M:%S)" >>"$CANARY_FILE"
			sleep 0.1
		done ) &
	CANARY_PID=$!
}
live_canary_stop() {
	[[ -n "$CANARY_PID" ]] || return 0
	kill "$CANARY_PID" 2>/dev/null
	wait "$CANARY_PID" 2>/dev/null
	CANARY_PID=""
	if [[ -s "$CANARY_FILE" ]]; then
		bad "live canary: the live focus sat on the gate's nested-dev output $(wc -l <"$CANARY_FILE") sample(s) during the run — the gate must never take live focus ($CANARY_FILE)"
		return 1
	fi
	ok "live canary: live focus never touched the gate's output"
	return 0
}

normalize_target_pkgconfig() {
	local pkg_path=${HYPR_DEPLOY_PKG_CONFIG_PATH:-}
	[[ -n "$pkg_path" ]] || return 0

	# The deploy path is intentionally one disposable package set. A colon
	# list could make pkg-config select a different hyprland.pc than the one
	# normalized here.
	if [[ "$pkg_path" == *:* ]]; then
		echo "HYPR_DEPLOY_PKG_CONFIG_PATH must name one pkg-config directory" >&2
		return 1
	fi

	local source_pc="$pkg_path/hyprland.pc"
	[[ -f "$source_pc" ]] || {
		echo "missing hyprland.pc under HYPR_DEPLOY_PKG_CONFIG_PATH: $pkg_path" >&2
		return 1
	}

	local prefix
	prefix="$(cd "$pkg_path/../.." 2>/dev/null && pwd)/include" || return 1
	[[ -d "$prefix/hyprland" ]] || {
		echo "missing target headers beside HYPR_DEPLOY_PKG_CONFIG_PATH: $prefix" >&2
		return 1
	}

	# CMake writes the configured install prefix into hyprland.pc even when
	# cmake --install is redirected to a disposable --prefix. Normalize an
	# owned copy, never metadata the caller passed to the gate.
	PKG_COPY_DIR="$(mktemp -d "$HARNESS/hypr-pkgconfig.XXXXXX")" || return 1
	cp -- "$source_pc" "$PKG_COPY_DIR/hyprland.pc" || return 1
	# The target pc's Requires must resolve inside the disposable set; a
	# dependency only present in a user-local tree (e.g. a freshly built
	# aquamarine) is invisible from the copy dir and pkg-config silently
	# falls back to the system version, failing the version check.
	local pc
	for pc in "$pkg_path"/*.pc; do
		[[ -f "$pc" && "$pc" != "$source_pc" ]] && cp -- "$pc" "$PKG_COPY_DIR/" || true
	done
	sed -i "s|^prefix=.*|prefix=$prefix|" "$PKG_COPY_DIR/hyprland.pc" || return 1
	HYPR_DEPLOY_PKG_CONFIG_PATH="$PKG_COPY_DIR"
	PKG_CONFIG_PATH="$PKG_COPY_DIR"
	export HYPR_DEPLOY_PKG_CONFIG_PATH PKG_CONFIG_PATH
}

validated_nested_pid() {
	[[ -n "$SIG" ]] || { echo "refusing hyprctl: nested signature is empty" >&2; return 1; }
	[[ "$SIG" =~ ^[[:alnum:]_.-]+$ ]] || { echo "refusing hyprctl: invalid nested signature" >&2; return 1; }
	[[ -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" || "$SIG" != "$HYPRLAND_INSTANCE_SIGNATURE" ]] || {
		echo "refusing hyprctl: nested signature resolves to the live compositor" >&2
		return 1
	}
	[[ -S "$RUNDIR/$SIG/.socket.sock" ]] || { echo "refusing hyprctl: nested control socket is missing" >&2; return 1; }

	local pid
	pid="$(head -n 1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
	[[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null || {
		echo "refusing hyprctl: nested lock does not identify a live process" >&2
		return 1
	}
	grep -Fzxq -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null || {
		echo "refusing hyprctl: target process does not own the stress harness config" >&2
		return 1
	}
	printf '%s\n' "$pid"
}

hq() {
	validated_nested_pid >/dev/null || return 1
	# bounded: a nested whose S1 handler blocks (e.g. a commit waiting on a
	# frame callback that never lands — the off-screen frame-cycle class)
	# must fail the checks, not hang the whole gate for minutes (2026-10-02:
	# an unbounded 'dsp ... & wait' close storm sat 8 min on this)
	timeout 10 hyprctl -i "$SIG" "$@"
}
dsp()     { hq dispatch "$1" >/dev/null 2>&1; }
clients() { hq clients -j 2>/dev/null; }
ws()      { hq activeworkspace -j | python3 -c 'import json,sys;print(json.load(sys.stdin)["id"])'; }
reserved() { hq monitors -j | python3 -c 'import json,sys;print(",".join(map(str,json.load(sys.stdin)[0]["reserved"])))'; }
hq_matches() { local pattern=$1; shift; hq "$@" | grep -qE "$pattern"; }
active_window_class_is() {
	hq activewindow -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)["class"] == sys.argv[1] else 1)' "$1"
}

# Nothing here may hard-code the nested monitor's size: it is whatever window
# the Wayland backend gets, and it can change when the nested config or its
# host surface is applied. retarget re-reads the instance after every launch
# and vp injects through vptr with that real extent — vptr maps coordinates as
# X/extent onto the output, so a stale extent silently lands every scripted
# click somewhere else and the assertion passes or fails on whatever happened
# to be under it.
WL=""; MON_W=0; MON_H=0; NBUS=""
retarget() {
	local n
	for n in 1 2 3; do
		retarget_env || { echo "retarget: nested state is unavailable" >&2; return 1; }
		# Every launch/relaunch passes through here: warm the parked window's
		# frame cycle before any battery takes a nested-side measurement.
		launch_warmup && return 0
		# The render cycle is dead and no focus kick revived it: relaunch the
		# nested instance (a fresh parking dance is a fresh draw) and retry.
		echo "harness: nested render cycle dead; relaunching nested (attempt $n/3)" >&2
		[[ $n == 3 ]] && return 1
		kill_nested
		launch_nested || { echo "retarget: relaunch FAILED" >&2; return 1; }
	done
	return 0
}

retarget_env() {
	SIG=""; WL=""; MON_W=0; MON_H=0; NBUS=""
	IFS= read -r SIG <"$HARNESS/nested.sig" 2>/dev/null || {
		echo "retarget: nested signature is unavailable" >&2
		return 1
	}
	IFS= read -r WL <"$HARNESS/nested.wl" 2>/dev/null || {
		echo "retarget: nested Wayland display is unavailable" >&2
		return 1
	}
	[[ -n "$WL" && "$WL" != */* ]] || {
		echo "retarget: nested Wayland display is invalid" >&2
		return 1
	}
	local pid dimensions
	pid="$(validated_nested_pid)" || return 1
	# launch.sh isolates the nested instance under its OWN dbus-run-session,
	# so anything driving the nested daemon over the bus must use THAT
	# address: the login session's bus is owned by the host's notification
	# daemon, which answers happily and makes the assertion vacuous.
	NBUS="$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')"
	[[ -n "$NBUS" ]] || { echo "retarget: nested D-Bus address is unavailable" >&2; return 1; }
	dimensions=""
	for _ in $(seq 1 100); do
		dimensions="$(hq monitors -j 2>/dev/null | python3 -c "
import json,sys
ms=json.load(sys.stdin)
if not ms: raise SystemExit(1)
m=ms[0]
print(int(m['width']/m['scale']), int(m['height']/m['scale']))" 2>/dev/null)" && [[ -n "$dimensions" ]] && break
		dimensions=""
		sleep 0.1
	done
	read -r MON_W MON_H <<<"$dimensions"
	[[ "$MON_W" =~ ^[1-9][0-9]*$ && "$MON_H" =~ ^[1-9][0-9]*$ ]] || {
		echo "retarget: nested monitor geometry is invalid" >&2
		return 1
	}
	return 0
}

# Render-cycle warmup: the parked window renders on the UNFOCUSED off-screen
# VM like any window there — damage on its surface schedules frames on the
# overlapping output (IHyprRenderer::damageSurface), and capture_nested's
# own vptr jitter (a motion INSIDE the nested, not the live session) pushes
# a frame on demand. A cold start can still lose the nested's first frame
# callback while it initializes; retrying the capture (which jitters) lets
# the cycle self-heal. NO live focus dispatches here: the old parking dance
# (focus the VM, hand focus back) stole the user's keyboard focus and yanked
# the mouse state on every launch/warmup (user rule 2026-10-02: the gate
# never takes the live mouse or focus).
launch_warmup() {
	local n
	for n in 1 2 3 4 5; do
		capture_nested "$STATE/launch-warmup.png" && return 0
		sleep 1
	done
	echo "harness: nested render cycle never warmed (captures starve); aborting" >&2
	# Post-mortem: is the nested alive, and what did it last log? A live
	# process with a silent log is a stalled frame cycle; a dead process
	# means the window died mid-park. (This used to be the only evidence left
	# to a failed gate run.)
	if [[ -n "${SIG:-}" ]]; then
		local npid
		npid="$(head -n 1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
		if [[ "${npid:-}" =~ ^[0-9]+$ ]] && kill -0 "$npid" 2>/dev/null; then
			echo "harness: nested $npid still alive; nested log tail:" >&2
		else
			echo "harness: nested process is GONE; nested log tail:" >&2
		fi
		grep -ivE "xkbcomp|Warning:|^>|DEBUG" "$HARNESS/nested.log" 2>/dev/null | tail -8 >&2
	fi
	return 1
}
vp() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1; }
vk() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vkbd" >/dev/null 2>&1; } # keys need no extent
capture_nested() { # capture_nested <output>: tolerate a transient screencopy denial
	local out=$1
	# The parked compositor renders on damage: start grim, then jitter the
	# virtual pointer so a frame is produced while the screencopy waits. An
	# idle nested session can otherwise starve the capture to a timeout.
	# The jitter MUST be a real delta: a fresh nested parks its pointer at
	# the output center, so the old absolute 'move 640 400' was a no-op with
	# no damage and the whole launch starved (17-min transparent window).
	# Alternate direction so repeated captures do not drift the pointer.
	local jitter_pid="" dir=1
	for _ in 1 2 3; do
		timeout 8 env WAYLAND_DISPLAY="$WL" grim "$out" >/dev/null 2>&1 &
		local gpid=$!
		( sleep 0.4; printf 'rel %d %d\nsleep 10\n' $((dir * 2)) $((dir * 2)) | vp ) &
		jitter_pid=$!
		dir=$((-dir))
		if wait "$gpid"; then
			wait "$jitter_pid" 2>/dev/null
			# A capture of a different size is a broken assumption, not a
			# transient error: every pixel metric derives from MON_WxMON_H, so
			# a drifted frame would poison downstream measurements silently.
			# Drop it and retry.
			python3 -c 'import sys
from PIL import Image
sys.exit(0 if Image.open(sys.argv[1]).size == (int(sys.argv[2]), int(sys.argv[3])) else 1)' "$out" "$MON_W" "$MON_H" 2>/dev/null || {
				rm -f -- "$out"
				continue
			}
			return 0
		fi
		kill "$jitter_pid" 2>/dev/null
		sleep 0.2
	done
	return 1
}
# pyc <python-expr-over-cs> — cs = client list; truthy stdout "1" = pass
pyc() { clients | python3 -c "
import json,sys
cs=json.load(sys.stdin)
print(1 if ($1) else 0)" ; }
expect() { # expect <name> <python-expr-over-cs>
	[[ "$(pyc "$2")" == "1" ]] && ok "$1" || bad "$1"
}

# Per-battery accounting: the gate brackets each battery with
# battery_begin/battery_end so the final summary reports the per-battery
# counts. A battery that silently lost checks (a skipped block, a relaunch
# that no-ops, a metric that started crashing) shows up as a lower count
# instead of a quiet green. The guard checks (assert_desktop_clean) are
# tracked separately: they are the ONLY checks a dead battery script can
# produce, so a battery with zero non-guard checks fails the gate.
BATTERY_NAME=""
BATTERY_SUMMARY=()
GUARD_CHECKS=0
GUARDS_START=0
battery_begin() {
	BATTERY_NAME=$1
	PASS_START=$PASS
	FAILED_START=${#FAILED[@]}
	GUARDS_START=$GUARD_CHECKS
	echo
	echo "== battery: $1 (${SECONDS} s in) =="
}
battery_end() {
	BATTERY_SUMMARY+=("$1 $((PASS - PASS_START)) $(( ${#FAILED[@]} - FAILED_START )) $((GUARD_CHECKS - GUARDS_START))")
}
# A battery must leave the nested client-free: the stress desktop starts
# empty and every battery closes its own windows. A stray client at a
# boundary is a leak by definition — the focus battery once left its foot at
# the bottom-right corner, under the tray menu column, and poisoned every
# panel-extent capture of the battery after it. Called from the gate
# loop BEFORE battery_end so the result counts into this battery; lifecycle
# is exempt (its tail tears the nested down itself, so the query is empty
# for the wrong reason).
assert_desktop_clean() {
	sleep 0.5
	local who
	who="$(clients | python3 -c 'import json,sys;print(" ".join(sorted({c["class"] for c in json.load(sys.stdin)})))' 2>/dev/null)"
	if [[ -z "$who" ]]; then
		ok "$1: leaves the nested client-free"
	else
		bad "$1: leaves nested clients behind: $who"
	fi
	GUARD_CHECKS=$((GUARD_CHECKS + 1))
}
print_summary() { # the gate's final lines; the return code is the exit code
	local entry n o f g real
	if [[ ${#BATTERY_SUMMARY[@]} -gt 0 ]]; then
		for entry in "${BATTERY_SUMMARY[@]}"; do
			read -r n o f g <<<"$entry"
			g=${g:-0}
			real=$((o + f - g))
			printf '   %-14s %s ok, %s fail%s\n' "$n" "$o" "$f" "$([[ $g -gt 0 ]] && printf ' (+%s guard)' "$g")"
			[[ $real -gt 0 ]] || bad "battery $n ran no checks (empty battery — script dead or fully gated)"
		done
	fi
	if [[ ${#FAILED[@]} -eq 0 ]]; then
		echo "== stress: ALL $PASS CHECKS PASSED in ${SECONDS}s =="
		return 0
	fi
	echo "== stress: $PASS passed, ${#FAILED[@]} FAILED in ${SECONDS}s =="
	printf '   - %s\n' "${FAILED[@]}"
	return 1
}

stop_capture() {
	if [[ -n "$CAPTURE_PID" ]]; then
		if grep -Fzxq -- "$REPO/devtools/input-capture" "/proc/$CAPTURE_PID/cmdline" 2>/dev/null; then
			kill "$CAPTURE_PID" 2>/dev/null || true
			wait "$CAPTURE_PID" 2>/dev/null || true
		fi
		CAPTURE_PID=""
	fi
}

kill_nested() { # kill any non-live instance running one of the harness cfgs
	stop_capture
	local killed="" pid
	# By cmdline, not by signature dir: a nested that boots slower than
	# launch.sh's signature wait outlives a gate that gave up on it — the
	# dir appears only when the boot completes, so a dir scan misses the
	# orphan (2026-10-01: two failed gdb launches left zombies hosting
	# aquamarine windows until a third nested joined and the live session
	# died with it). The live instance never carries a harness cfg, so the
	# match is live-safe without the signature compare.
	for pid in $(pgrep -x Hyprland 2>/dev/null); do
		grep -Fzxq -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null && { kill "$pid" 2>/dev/null; killed="$killed $pid"; continue; }
		# Same rule as stop.sh: any nested-style launch (-c, no live
		# --watchdog-fd) is a harness orphan, even when its cfg is not the
		# current one (2026-10-02: a /tmp debug-cfg orphan outlived every
		# stop; its window re-mapped onto eDP-1 and stole the live focus).
		if grep -Fzxq -- "-c" "/proc/$pid/cmdline" 2>/dev/null \
			&& ! grep -Fzxq -- "--watchdog-fd" "/proc/$pid/cmdline" 2>/dev/null; then
			kill "$pid" 2>/dev/null; killed="$killed $pid"
		fi
	done
	# Host-side client teardown outlives process death: a still-alive nested
	# during a live 'output remove' is the race that leaves the monitor half
	# torn down (zombie in the all-monitors list). Wait for full death, with
	# a SIGKILL fallback, before the caller proceeds.
	local k
	for k in $killed; do
		local _ sigkilled=0
		for _ in $(seq 1 50); do
			kill -0 "$k" 2>/dev/null || break
			sleep 0.1
		done
		if kill -0 "$k" 2>/dev/null; then
			echo "harness: warning: nested $k survived 5s, SIGKILL" >&2
			kill -9 "$k" 2>/dev/null
			for _ in $(seq 1 20); do
				kill -0 "$k" 2>/dev/null || break
				sleep 0.1
			done
			sigkilled=1
		fi
		nested_exit_check "$k" "$sigkilled"
	done
	sleep 0.6
}

nested_exit_check() { # $1=killed pid, $2=1 if SIGKILL was needed
	# The gate relaunches its nested without ever asking how the previous
	# instance died: the teardown SEGV class (use-after-dlclose, fork fix
	# 377b812e; 100% repro on every relaunch) passed every gate from
	# 2026-09-04 through 2026-09-25 because nothing looked at the corpse.
	# A clean SIGTERM exit leaves no core; a SEGV-class death does
	# (kernel.core_pattern pipes to systemd-coredump). An instance that had
	# to be SIGKILL'd can never have written a core — that is a teardown
	# hang, reported as such, not as a clean exit.
	if [[ "${2:-0}" == "1" ]]; then
		bad "nested $1 teardown: survived 5s and was SIGKILL'd (hang — no core to verify)"
		return 0
	fi
	local pattern
	pattern="$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
	if [[ "$pattern" != *systemd-coredump* ]]; then
		ok "nested $1 teardown: core check unverified (core_pattern is not the systemd-coredump pipe — reboot resets it)"
		return 0
	fi
	if ! command -v coredumpctl >/dev/null 2>&1; then
		ok "nested $1 teardown: core check unverified (no coredumpctl)"
		return 0
	fi
	# systemd-coredump records asynchronously; query, retry once on an
	# unreadable journal, then decide: 0=core, 1=clean, 2=unreadable.
	local out rc=2
	for _ in 1 2; do
		sleep 2
		out="$(coredumpctl list --json=short 2>/dev/null)"
		printf '%s' "$out" | python3 -c '
import json, sys
try:
    entries = json.load(sys.stdin)
except Exception:
    sys.exit(2)
sys.exit(0 if any(e.get("pid") == int(sys.argv[1]) for e in entries) else 1)' "$1"
		rc=$?
		[[ $rc -eq 2 ]] || break
	done
	case $rc in
		0) bad "nested $1 teardown: core dump found (SEGV class — coredumpctl info $1)" ;;
		1) ok "nested $1 teardown: clean exit, no core" ;;
		*) ok "nested $1 teardown: core check unverified (coredumpctl unavailable)" ;;
	esac
}

nested_dev_state() { # echo none | active | zombie (see remove_nested_dev)
	local all active
	all="$(hlq monitors all -j 2>/dev/null | python3 -c 'import json,sys;print(any(m["name"]=="nested-dev" for m in json.load(sys.stdin)))' 2>/dev/null)"
	active="$(hlq monitors -j 2>/dev/null | python3 -c 'import json,sys;print(any(m["name"]=="nested-dev" for m in json.load(sys.stdin)))' 2>/dev/null)"
	if [[ "$all" == True && "$active" == True ]]; then
		echo active
	elif [[ "$all" == True ]]; then
		echo zombie
	else
		echo none
	fi
}

nested_dev_occupants() { # classes of live windows mapped on nested-dev
	local mindex
	mindex="$(hlq monitors -j 2>/dev/null | python3 -c 'import json,sys;print(next((i for i,m in enumerate(json.load(sys.stdin),1) if m["name"]=="nested-dev"),0))' 2>/dev/null)"
	[[ "${mindex:-0}" != "0" ]] || return 0
	hlq clients -j 2>/dev/null | python3 -c "
import json, sys
print(' '.join(c['class'] for c in json.load(sys.stdin) if c.get('monitor') == $mindex))" 2>/dev/null
}

remove_nested_dev() { # remove + verify the monitor is fully gone; 0 clean, 1 leak
	local attempt state occ
	# never remove an output that still hosts live windows: nested-dev is the
	# gate's headless parking output, and anything parked there at sweep time
	# is the user's (a re-logged browser), not the gate's — the gate's own
	# nested window dies with its instance
	occ="$(nested_dev_occupants)"
	if [[ -n "$occ" ]]; then
		echo "harness: nested-dev has windows parked on it ($occ); refusing to remove" >&2
		return 1
	fi
	for attempt in 1 2; do
		hlq output remove nested-dev >/dev/null 2>&1
		for _ in $(seq 1 20); do
			state="$(nested_dev_state)"
			[[ "$state" == none ]] && return 0
			sleep 0.25
		done
	done
	return 1
}



# Launch readiness: the panel column must be clear before the first
# measurement, or a short first run at y26 reads as a 13px panel. The fork's
# 15s no-watchdog toast used to span exactly this column and poisoned every
# shade measurement until it faded; write_stress_cfg now suppresses it
# (misc:disable_watchdog_warning), so this normally clears within a couple of
# frames of launch. Only meaningful directly after launch_nested.
wait_launch_toast() {
	local f="$STATE/toast-check.png"
	for _ in $(seq 1 40); do
		capture_nested "$f" || { sleep 0.5; continue; }
		python3 - "$f" "$MON_W" <<'PY' && return 0
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0 = int(sys.argv[2]) - 392 + 2
x1 = int(sys.argv[2]) - 12
clear = all(min(px[x, y]) <= 5 for y in range(26, 64) for x in range(x0, x1, 3))
raise SystemExit(0 if clear else 1)
PY
		sleep 0.5
	done
	echo "panel column never cleared after launch; shade measurements would be poisoned" >&2
	return 1
}

launch_nested() {
	# A failed run's orphaned nested (boot slower than the signature wait)
	# has no registerable state; sweep by cfg before spawning another, or
	# two harness nested coexist — their extra divergent nested-dev outputs
	# are the live-session kill recipe (2026-10-01).
	live_canary_start
	kill_nested
	if [[ -z "$HARNESS_OUTPUT_OWNED" ]]; then
		HARNESS_OUTPUT_OWNED=0
		case "$(nested_dev_state)" in
			none)
				HARNESS_OUTPUT_OWNED=1
				: > "$HARNESS/nested.output-owned"
				;;
			zombie)
				# A prior crashed/leaked run left it in the all-monitors list
				# only; clean it before claiming, or refuse to launch on top.
				if remove_nested_dev; then
					HARNESS_OUTPUT_OWNED=1
					: > "$HARNESS/nested.output-owned"
				else
					echo "harness: WARNING: leaked 'nested-dev' monitor could not be removed; move/close its windows, then run 'hyprctl output remove nested-dev' (or relog)" >&2
					return 1
				fi
				;;
			active)
				# present and alive: only claim it when the marker says this
				# harness family created it, otherwise another nested owns it
				if [[ -e "$HARNESS/nested.output-owned" ]]; then
					HARNESS_OUTPUT_OWNED=1
				fi
				;;
		esac
	fi
	# One spawn path for the initial launch AND the FALLBACK relaunches below:
	# the env must travel with the relaunch too, or it falls back to its
	# defaults (the gate's nested.lua + /usr/local/bin/Hyprland) and the
	# retarget guard refuses it.
	_harness_launch() {
		PATH="$REPO/devtools/fakes:$PATH" AW_WPCTL_LOG="$STATE/wpctl.log" \
			AW_WPCTL_HANG_FILE="$STATE/hang-wpctl" AW_WPCTL_FLOOD_FILE="$STATE/flood-wpctl" AW_SOUND_HANG_FILE="$STATE/hang-sound" \
			HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" XDG_CACHE_HOME="$STATE/cache" \
			bash "$GATE_DIR/launch.sh" >>"$HARNESS/launch.log" 2>&1
	}
	# The launch diagnostics used to be swallowed here; a failed park or a
	# dead nested is only readable from that log.
	_harness_launch || {
		echo "harness: nested launch failed — launch log tail:" >&2
		tail -25 "$HARNESS/launch.log" 2>/dev/null >&2
		return 1
	}
	# The nested compositor enters fallback (a headless "FALLBACK" output,
	# no window) 2s after its ready event when its aquamarine window output
	# never appeared. A window lost in that window renders into the void:
	# every nested capture starves and all batteries fail. A late window
	# output can still arrive and lift the fallback, so give it a moment
	# before relaunching (launch.sh kills the prior same-config nested).
	local sig n
	sig="$(cat "$HARNESS/nested.sig" 2>/dev/null)"
	[[ -n "$sig" ]] || return 1
	for n in 1 2 3; do
		for _ in $(seq 1 10); do
			hyprctl -i "$sig" monitors -j 2>/dev/null | python3 -c 'import json,sys;ms=json.load(sys.stdin);sys.exit(0 if any(m["name"]!="FALLBACK" for m in ms) else 1)' && return 0
			sleep 1
		done
		echo "harness: nested stuck in FALLBACK (window lost?); relaunching (attempt $n/3)" >&2
		[[ $n == 3 ]] && return 1
		_harness_launch || return 1
		sig="$(cat "$HARNESS/nested.sig" 2>/dev/null)"
		sleep 2
	done
	return 0
}

cleanup_harness_core() {
	[[ "${HARNESS_CLEANED:-0}" == 1 ]] && return
	HARNESS_CLEANED=1
	stop_capture
	if [[ -n "${CLIP_PID:-}" ]]; then
		if grep -Fzxq -- "$REPO/devtools/cliphold" "/proc/$CLIP_PID/cmdline" 2>/dev/null; then
			kill "$CLIP_PID" 2>/dev/null || true
			wait "$CLIP_PID" 2>/dev/null || true
		fi
		CLIP_PID=""
	fi
	for marker in "$STATE/hang-wpctl" "$STATE/hang-sound"; do
		local pid
		pid="$(cat "${marker}.pid" 2>/dev/null)"
		if [[ "$pid" =~ ^[0-9]+$ ]] && grep -Fzxq -- "XDG_STATE_HOME=$STATE" "/proc/$pid/environ" 2>/dev/null; then
			kill "$pid" 2>/dev/null || true
		fi
	done
	kill_nested
	if [[ "${HYPR_STRESS_KEEP_STATE:-0}" == 1 ]]; then
		echo "   retained nested evidence under $STATE"
	else
		# A crash report under $STATE is the only evidence of HOW the nested
		# died; the rm below would destroy it (2026-10-01: a red run in the
		# repeat-backpressure burst left exactly one). $HARNESS survives the
		# next run's fresh_stress_state.
		local report
		for report in "$STATE"/cache/hyprland/hyprlandCrashReport*.txt; do
			[[ -f "$report" ]] || continue
			mkdir -p "$HARNESS/crash-reports"
			cp -- "$report" "$HARNESS/crash-reports/" 2>/dev/null &&
				echo "   retained crash report: $HARNESS/crash-reports/$(basename "$report")" >&2
		done
		rm -rf -- "$STATE" "$CFG"
	fi
	if [[ "${HARNESS_OUTPUT_OWNED:-0}" == 1 ]]; then
		rm -f -- "$HARNESS/nested.output-owned"
		if ! remove_nested_dev; then
			echo "harness: WARNING: 'nested-dev' monitor survived output removal (zombie); live by-id workspace lookups may misroute — run 'hyprctl output remove nested-dev' or relog" >&2
		fi
	fi
	live_canary_stop
	rm -f -- "$HARNESS/nested.sig" "$HARNESS/nested.wl"
	if [[ -n "${PKG_COPY_DIR:-}" && "$PKG_COPY_DIR" == "$HARNESS"/hypr-pkgconfig.* ]]; then
		rm -rf -- "$PKG_COPY_DIR"
		PKG_COPY_DIR=""
	fi
}

# ---- the monolith's harness state ------------------------------------------
#
# The plugin's single persistent state file (core/state.hpp): one typed
# file, one load, one save path. The legacy sources it migrates — and
# CONSUMES — from are seeded by fresh_stress_state, so every full run
# exercises the one-time migration.
AWSTATE="$STATE/hyprland/plugin/awesome"
AW_STATE="$AWSTATE/state.tsv"
LEG_SPOT="$STATE/hyprplace/lastspot.tsv"
LEG_WINDOWED="$STATE/hyprmax/windowed.tsv"
LEG_MENUBAR_COUNT="$STATE/cache/hyprbar/menu_count_file"
LEG_MENUBAR_HIST="$STATE/cache/hyprbar/history_menu"

# The default stress state seeds the ANCIENT legacy store: the monolith's
# first init migrates it into $AW_STATE (and consumes the source), and the
# placement batteries read the unified file afterwards. (The "exactly one
# foot row" check reads the spot rows of $AW_STATE, where the migration
# consolidated the legacy rows.)
fresh_stress_state() {
	rm -rf -- "$STATE"
	mkdir -p "$(dirname "$LEG_SPOT")" "$(dirname "$LEG_WINDOWED")" "$(dirname "$LEG_MENUBAR_COUNT")"
	printf '100\t100\t500\t400\tfoot\n200\t80\tlegacyfoot\n' > "$LEG_SPOT"
}

# The system battery's brightness presses move the HOST backlight (the
# nested's logind session is the live session's). The battery writes a
# restore marker; this runs it before the core teardown wipes $STATE, so the
# effect is reversible even on a mid-battery failure. (The core teardown is
# called by its own name — a wrapper that re-invoked `cleanup_harness`
# would resolve to itself at call time and recurse into a stack overflow,
# which is exactly how the pre-merge harness died with RC=139 on every
# run's exit trap.)
cleanup_harness() {
	local f="${STATE:-}/sys-brightness-restore"
	if [[ -n "${STATE:-}" && -f "$f" ]]; then
		local dev val sess
		read -r dev val <"$f"
		if [[ -n "$dev" && -w "/sys/class/backlight/$dev/brightness" ]]; then
			printf '%s' "$val" > "/sys/class/backlight/$dev/brightness" 2>/dev/null || true
		else
			sess="$(sed -n 's/.*session-\([0-9]*\)\.scope.*/\1/p' /proc/self/cgroup | head -1)"
			[[ -n "$sess" ]] && busctl --user call org.freedesktop.login1 \
				"/org/freedesktop/login1/session/$sess" \
				org.freedesktop.login1.Session SetBrightness ssu "backlight" "$dev" "$val" >/dev/null 2>&1 || true
		fi
	fi
	cleanup_harness_core
}

write_stress_cfg() {
	{
		echo 'hl.config({ ecosystem = { enforce_permissions = true } })'
		echo 'hl.permission(".*hyprland-plugins/.*", "plugin", "allow")'
		echo 'hl.permission(".*input-capture$", "input-capture", "allow")'
		echo 'hl.permission(".*vkbd$", "keyboard", "allow")'
		echo 'hl.permission(".*grim$", "screencopy", "allow")'
		awk '{ print } /misc = \{/ { print "\t\tdisable_watchdog_warning = 1," }' "$GATE_DIR/nested.lua"
		echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs|csdfollow|csdfall" }, float = true })'
	} > "$CFG"
}
