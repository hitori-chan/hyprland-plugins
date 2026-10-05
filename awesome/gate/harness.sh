# SECONDS-based wall clock, in s since gate.sh started: each line shows
# where the minutes go, so a slow battery is visible without a profiler.
ok()  { PASS=$((PASS + 1)); printf '  ok  [%s s] %s\n' "$SECONDS" "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL [%s s] %s\n' "$SECONDS" "$1"; }
chk() { # chk <name> <command...> — command's exit code decides
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
}

# --- the live session: read-only --------------------------------------------
# The nested runs inside a private host (host.sh): the harness sends the
# live session NOTHING — no output, workspace, window, dispatch, focus or
# cursor (user rule, absolute: tests never touch the live desktop). It
# reads it twice, for the isolation check below.
HARNESS_DIR="$HARNESS"
# shellcheck source=live.sh
. "$GATE_DIR/live.sh" # LIVE_SIG/LIVE_PID, live_read, is_nested_pid, nested_log, sweep_nested_dirs
# shellcheck source=host.sh
. "$GATE_DIR/host.sh" # host_*, sysbus_*, aq_pinned_libdir

# The isolation check: the structural half runs at every launch (the
# nested's parent display is the private host's socket, never the live
# one — refuse to continue otherwise); the observed half compares the live
# session's outputs and nested-class windows before and after the run.
LIVE_MONITORS_AT_START=""
isolation_start() {
	[[ -n "$LIVE_MONITORS_AT_START" ]] && return
	LIVE_MONITORS_AT_START="$(live_read monitors all -j 2>/dev/null | python3 -c 'import json,sys;print(" ".join(sorted(m["name"] for m in json.load(sys.stdin))))' 2>/dev/null)"
}
isolation_verify_nested() { # <pid>: its parent display must be the private host
	local pid=$1 parent host
	host="$(cat "$HOST_DIR/wl" 2>/dev/null)"
	parent="$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^WAYLAND_DISPLAY=//p')"
	if [[ -z "$host" || "$parent" != "$host" || "$parent" == "${WAYLAND_DISPLAY:-wayland-1}" ]]; then
		bad "isolation: nested $pid's parent display is '$parent', not the private host '$host' — refusing to run against the live session"
		return 1
	fi
}
isolation_check() {
	[[ -n "$LIVE_SIG" ]] || { ok "isolation: no live session to compare (not run inside one)"; return 0; }
	local now leaked
	now="$(live_read monitors all -j 2>/dev/null | python3 -c 'import json,sys;print(" ".join(sorted(m["name"] for m in json.load(sys.stdin))))' 2>/dev/null)"
	leaked="$(live_read clients -j 2>/dev/null | python3 -c 'import json,sys;print(" ".join(c["class"] for c in json.load(sys.stdin) if c["class"] in ("aquamarine","labwc")))' 2>/dev/null)"
	if [[ -n "$LIVE_MONITORS_AT_START" && "$now" != "$LIVE_MONITORS_AT_START" ]]; then
		bad "isolation: the live outputs changed during the run ($LIVE_MONITORS_AT_START -> $now)"
	elif [[ -n "$leaked" ]]; then
		bad "isolation: nested windows appeared in the live session ($leaked)"
	else
		ok "isolation: the live session was untouched (outputs: ${now:-?})"
	fi
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
	# frame callback that never lands — a starved frame cycle)
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
		# Every launch/relaunch passes through here: warm the nested's frame
		# cycle before any battery takes a nested-side measurement.
		launch_warmup && return 0
		# The render cycle is dead: relaunch the nested instance (a fresh
		# host window is a fresh draw) and retry.
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

# Render-cycle warmup: the nested's host window renders on damage like any
# window, and capture_nested's own vptr jitter (a motion inside the nested)
# pushes a frame on demand. A cold start can still lose the nested's first
# frame callback while it initializes; retrying the capture (which jitters)
# lets the cycle self-heal.
launch_warmup() {
	local n
	for n in 1 2 3 4 5; do
		capture_nested "$STATE/launch-warmup.png" && return 0
		sleep 1
	done
	echo "harness: nested render cycle never warmed (captures starve); aborting" >&2
	# Post-mortem: is the nested alive, and what did it last log? A live
	# process with a silent log is a stalled frame cycle; a dead process
	# means the instance died at boot. (This used to be the only evidence left
	# to a failed gate run.)
	if [[ -n "${SIG:-}" ]]; then
		local npid
		npid="$(head -n 1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
		if [[ "${npid:-}" =~ ^[0-9]+$ ]] && kill -0 "$npid" 2>/dev/null; then
			echo "harness: nested $npid still alive; nested log tail:" >&2
		else
			echo "harness: nested process is GONE; nested log tail:" >&2
		fi
		grep -ivE "xkbcomp|Warning:|^>|DEBUG" "$(nested_log)" "$HARNESS/nested.stdout" 2>/dev/null | tail -8 >&2
	fi
	return 1
}
vp() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1; }
vk() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vkbd" >/dev/null 2>&1; } # keys need no extent
capture_nested() { # capture_nested <output>: tolerate a transient screencopy denial
	local out=$1
	# The nested renders on damage: start grim, then jitter the
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
	# Same rule as stop.sh: any nested-style launch (-c, no --watchdog-fd,
	# never the live PID) is a harness orphan, even when its cfg is not the
	# current one (2026-10-02: a /tmp debug-cfg orphan outlived every stop;
	# its window re-mapped onto the live panel and stole the focus).
	for pid in $(pgrep -x Hyprland 2>/dev/null); do
		is_nested_pid "$pid" && { kill "$pid" 2>/dev/null; killed="$killed $pid"; }
	done
	# Wait for full death, with a SIGKILL fallback, before the caller
	# relaunches: a dying nested still holding its host window races the
	# next one's boot.
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
	sweep_nested_dirs # their runtime dirs, keeping the last log
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
	isolation_start
	kill_nested
	# One spawn path for the initial launch AND the FALLBACK relaunches below:
	# the env travels with every relaunch (the fake wpctl/sound PATH, the
	# AW_* hang/flood files, the scratch XDG dirs, the fake backlight).
	_harness_launch() {
		PATH="$REPO/devtools/fakes:$PATH" AW_WPCTL_LOG="$STATE/wpctl.log" \
			AW_WPCTL_HANG_FILE="$STATE/hang-wpctl" AW_WPCTL_FLOOD_FILE="$STATE/flood-wpctl" AW_SOUND_HANG_FILE="$STATE/hang-sound" \
			AW_BACKLIGHT_DIR="$STATE/backlight" AW_LOGIND_LOG="$STATE/logind.log" \
			HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" XDG_CACHE_HOME="$STATE/cache" XDG_CONFIG_HOME="$STATE/config" \
			bash "$GATE_DIR/launch.sh" >>"$HARNESS/launch.log" 2>&1
	}
	_harness_verify() {
		local sig pid
		sig="$(cat "$HARNESS/nested.sig" 2>/dev/null)"
		pid="$(head -n 1 "$RUNDIR/$sig/hyprland.lock" 2>/dev/null)"
		[[ "$pid" =~ ^[0-9]+$ ]] || { echo "harness: no nested pid for $sig" >&2; return 1; }
		isolation_verify_nested "$pid"
	}
	_harness_launch && _harness_verify || {
		echo "harness: nested launch failed — launch log tail:" >&2
		tail -25 "$HARNESS/launch.log" 2>/dev/null >&2
		return 1
	}
	# The nested compositor enters fallback (a headless "FALLBACK" output,
	# no window) 2s after its ready event when its host window output never
	# appeared; every capture would starve. A late window output can still
	# lift the fallback, so give it a moment before relaunching.
	local sig n
	sig="$(cat "$HARNESS/nested.sig" 2>/dev/null)"
	[[ -n "$sig" ]] || return 1
	for n in 1 2 3; do
		for _ in $(seq 1 10); do
			hyprctl -i "$sig" monitors -j 2>/dev/null | python3 -c 'import json,sys;ms=json.load(sys.stdin);sys.exit(0 if any(m["name"]!="FALLBACK" for m in ms) else 1)' && return 0
			sleep 1
		done
		echo "harness: nested stuck in FALLBACK (host window lost?); relaunching (attempt $n/3)" >&2
		[[ $n == 3 ]] && return 1
		_harness_launch && _harness_verify || return 1
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
	host_stop
	sysbus_stop
	isolation_check
	rm -f -- "$HARNESS/nested.sig" "$HARNESS/nested.wl"
	if [[ -n "${PKG_COPY_DIR:-}" && "$PKG_COPY_DIR" == "$HARNESS"/hypr-pkgconfig.* ]]; then
		rm -rf -- "$PKG_COPY_DIR"
		PKG_COPY_DIR=""
	fi
}

# ---- the monolith's harness state ------------------------------------------
#
# The plugin's single persistent state file (core/state.hpp). The stress
# state seeds one remembered spot, so the first spawn of the batteries
# proves the load: foot's spot is (100,100).
AWSTATE="$STATE/hyprland/plugin/awesome"
AW_STATE="$AWSTATE/state.tsv"

fresh_stress_state() {
	rm -rf -- "$STATE"
	mkdir -p "$AWSTATE" "$STATE/config"
	# The nested session's own config home: real apps run in it (Thunar,
	# GTK pickers, Firefox) and wrote their window sizes into the user's
	# xfconf/dconf (2026-10-04). Fonts stay the user's: text metrics are
	# part of what the batteries measure.
	[[ -d "${XDG_CONFIG_HOME:-$HOME/.config}/fontconfig" ]] && ln -s "${XDG_CONFIG_HOME:-$HOME/.config}/fontconfig" "$STATE/config/fontconfig"
	printf 'spot\t100\t100\tfoot\n' > "$AW_STATE"
}

# The exit trap's name. (The core teardown is called by its own name — a
# wrapper that re-invoked `cleanup_harness` would resolve to itself at call
# time and recurse into a stack overflow: the pre-merge harness died with
# RC=139 on every run's exit trap that way.)
cleanup_harness() {
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
		echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs|csdfollow|csdfall|csdmem|csdsmall|exitfoot" }, float = true })'
	} > "$CFG"
}
