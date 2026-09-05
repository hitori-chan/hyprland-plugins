# SECONDS-based wall clock, in s since stress.sh started: each line shows
# where the minutes go, so a slow battery is visible without a profiler.
ok()  { PASS=$((PASS + 1)); printf '  ok  [%s s] %s\n' "$SECONDS" "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL [%s s] %s\n' "$SECONDS" "$1"; }
chk() { # chk <name> <command...> — command's exit code decides
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
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
	hyprctl -i "$SIG" "$@"
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
	# address: the login session's bus is owned by the host's hyprnotify,
	# which answers happily and makes the assertion vacuous.
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
}
vp() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1; }
vk() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vkbd" >/dev/null 2>&1; } # keys need no extent
capture_nested() { # capture_nested <output>: tolerate a transient screencopy denial
	local out=$1
	# The parked compositor renders on damage: start grim, then jitter the
	# virtual pointer so a frame is produced while the screencopy waits. An
	# idle nested session can otherwise starve the capture to a timeout.
	local jitter_pid=""
	for _ in 1 2 3; do
		timeout 8 env WAYLAND_DISPLAY="$WL" grim "$out" >/dev/null 2>&1 &
		local gpid=$!
		( sleep 0.4; printf 'move %d %d\nsleep 10\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp ) &
		jitter_pid=$!
		if wait "$gpid"; then
			wait "$jitter_pid" 2>/dev/null
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
	local killed="" s sig pid
	for s in "$RUNDIR"/*/; do
		sig="$(basename "$s")"
		[[ "$sig" == "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] && continue
		pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
		[[ -n "$pid" ]] || continue
		grep -Fzxq -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null && { kill "$pid" 2>/dev/null; killed="$killed $pid"; }
	done
	# Host-side client teardown outlives process death: a still-alive nested
	# during a live 'output remove' is the race that leaves the monitor half
	# torn down (zombie in the all-monitors list). Wait for full death, with
	# a SIGKILL fallback, before the caller proceeds.
	local k
	for k in $killed; do
		local _
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
		fi
	done
	sleep 0.6
}

nested_dev_state() { # echo none | active | zombie (see remove_nested_dev)
	local all active
	all="$(hyprctl monitors all -j 2>/dev/null | python3 -c 'import json,sys;print(any(m["name"]=="nested-dev" for m in json.load(sys.stdin)))' 2>/dev/null)"
	active="$(hyprctl monitors -j 2>/dev/null | python3 -c 'import json,sys;print(any(m["name"]=="nested-dev" for m in json.load(sys.stdin)))' 2>/dev/null)"
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
	mindex="$(hyprctl monitors -j 2>/dev/null | python3 -c 'import json,sys;print(next((i for i,m in enumerate(json.load(sys.stdin),1) if m["name"]=="nested-dev"),0))' 2>/dev/null)"
	[[ "${mindex:-0}" != "0" ]] || return 0
	hyprctl clients -j 2>/dev/null | python3 -c "
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
		hyprctl output remove nested-dev >/dev/null 2>&1
		for _ in $(seq 1 20); do
			state="$(nested_dev_state)"
			[[ "$state" == none ]] && return 0
			sleep 0.25
		done
	done
	return 1
}

fresh_stress_state() { # a clean fixture state with hyprplace's seeded spots
	rm -rf -- "$STATE"
	mkdir -p "$STATE/hyprplace"
	printf '100\t100\t500\t400\tfoot\n200\t80\tlegacyfoot\n' > "$STATE/hyprplace/lastspot.tsv"
}

write_stress_cfg() { # the nested config: nested.lua plus the stress preamble
	{
		echo 'hl.config({ ecosystem = { enforce_permissions = true } })'
		echo 'hl.permission(".*hyprland-plugins/.*", "plugin", "allow")'
		echo 'hl.permission(".*input-capture$", "input-capture", "allow")'
		echo 'hl.permission(".*vkbd$", "keyboard", "allow")'
		echo 'hl.permission(".*grim$", "screencopy", "allow")'
		# Suppress the fork's 15s no-watchdog toast (it lives in the panel
		# column, so wait_launch_toast must otherwise poll for its whole
		# lifetime at every launch). Injected into nested.lua's own misc
		# table — a second hl.config call's merge semantics are not a
		# contract. A harness config without a misc table keeps the toast
		# and pays the wait; the gate still passes.
		awk '{ print } /misc = \{/ { print "\t\tdisable_watchdog_warning = 1," }' "$HARNESS/nested.lua"
		echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs" }, float = true })'
	} > "$CFG"
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
	PATH="$REPO/devtools/fakes:$PATH" HYPROSD_WPCTL_LOG="$STATE/wpctl.log" \
		HYPROSD_WPCTL_HANG_FILE="$STATE/hang-wpctl" HYPROSD_WPCTL_FLOOD_FILE="$STATE/flood-wpctl" HYPRNOTIFY_SOUND_HANG_FILE="$STATE/hang-sound" \
		HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" XDG_CACHE_HOME="$STATE/cache" \
		bash "$HARNESS/launch.sh" >/dev/null 2>&1
}

cleanup_harness() {
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
		rm -rf -- "$STATE" "$CFG"
	fi
	if [[ "${HARNESS_OUTPUT_OWNED:-0}" == 1 ]]; then
		rm -f -- "$HARNESS/nested.output-owned"
		if ! remove_nested_dev; then
			echo "harness: WARNING: 'nested-dev' monitor survived output removal (zombie); live by-id workspace lookups may misroute — run 'hyprctl output remove nested-dev' or relog" >&2
		fi
	fi
	rm -f -- "$HARNESS/nested.sig" "$HARNESS/nested.wl"
	if [[ -n "${PKG_COPY_DIR:-}" && "$PKG_COPY_DIR" == "$HARNESS"/hypr-pkgconfig.* ]]; then
		rm -rf -- "$PKG_COPY_DIR"
		PKG_COPY_DIR=""
	fi
}
