ok()  { PASS=$((PASS + 1)); printf '  ok  %s\n' "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL %s\n' "$1"; }
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
	for _ in 1 2 3; do
		timeout 8 env WAYLAND_DISPLAY="$WL" grim "$out" >/dev/null 2>&1 && return 0
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
	for s in "$RUNDIR"/*/; do
		local sig pid
		sig="$(basename "$s")"
		[[ "$sig" == "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] && continue
		pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
		[[ -n "$pid" ]] || continue
		grep -Fzxq -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null && kill "$pid" 2>/dev/null
	done
	sleep 0.6
}

launch_nested() {
	if [[ -z "$HARNESS_OUTPUT_OWNED" ]]; then
		HARNESS_OUTPUT_OWNED=0
		if [[ -e "$HARNESS/nested.output-owned" ]] || ! hyprctl monitors -j 2>/dev/null | python3 -c 'import json,sys;sys.exit(0 if any(m["name"] == "nested-dev" for m in json.load(sys.stdin)) else 1)'; then
			HARNESS_OUTPUT_OWNED=1
			: > "$HARNESS/nested.output-owned"
		fi
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
		hyprctl output remove nested-dev >/dev/null 2>&1 || true
		rm -f -- "$HARNESS/nested.output-owned"
	fi
	rm -f -- "$HARNESS/nested.sig" "$HARNESS/nested.wl"
	if [[ -n "${PKG_COPY_DIR:-}" && "$PKG_COPY_DIR" == "$HARNESS"/hypr-pkgconfig.* ]]; then
		rm -rf -- "$PKG_COPY_DIR"
		PKG_COPY_DIR=""
	fi
}
