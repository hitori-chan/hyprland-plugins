#!/usr/bin/env bash
# awesome/gate/launch.sh — start the nested Hyprland dev instance, isolated
# from the user's session. The gate's harness calls this (harness.sh); it
# is also the standalone dev launcher.
#
# Nothing of the live session is used: the nested's parent display is a
# private headless labwc (host.sh), not the user's compositor — no output,
# workspace, window, focus or cursor there, and no live dispatch at all.
# The nested runs under
#   * a private session bus (dbus-run-session): its notification daemon
#     and SNI watcher own their names there, and every bus-activated
#     service (portals, dconf) sees the NESTED display, never the host's
#     or the user's;
#   * a private system bus with a fake logind: the brightness path runs
#     its real D-Bus calls against a fake backlight, never the panel;
#   * the caller's XDG_STATE/CACHE/CONFIG_HOME (the harness points them at
#     its scratch state, so real apps in the nested keep their settings
#     there, not in the user's xfconf/dconf).
#
# Runtime state (nested.sig / nested.wl / nested.stdout, the host, the
# system bus) goes to $HYPR_HARNESS, default ~/.local/share/hypr-nested.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
mkdir -p "$HARNESS_DIR"
CFG="${HYPR_CFG:-$HERE/nested.lua}"
# $1 (a manual launch) overrides HYPR_BIN; the harness passes HYPR_BIN and no
# argument. Refusing a missing binary keeps a mistyped path from silently
# launching the installed one (2026-10-02: manual probes ran the stale
# installed binary for an hour because this fell back by default).
BIN="${1:-${HYPR_BIN:-/usr/local/bin/Hyprland}}"
[[ -x "$BIN" ]] || { echo "launch: no such binary: $BIN" >&2; exit 1; }
# the nested's stdout/stderr: the boot lines before the config turns stdout
# logging off, plus crash output. Its real log is the instance's
# hyprland.log (live.sh: nested_log; kept as $HARNESS/nested.log once the
# instance is gone).
LOG="$HARNESS_DIR/nested.stdout"
[[ -f "$LOG" ]] && mv -f "$LOG" "$LOG.prev"

# shellcheck source=live.sh
. "$HERE/live.sh" # RUNDIR, LIVE_PID (read-only), is_nested_pid, sweep_nested_dirs
# shellcheck source=host.sh
. "$HERE/host.sh" # host_start, aq_pinned_libdir, sysbus_start

# --- end any prior nested (any harness config, never the live PID) --------
KILLED=""
for pid in $(pgrep -x Hyprland 2>/dev/null); do
	if is_nested_pid "$pid"; then
		echo "launch: killing prior nested (pid $pid)"
		kill "$pid" 2>/dev/null
		KILLED="$KILLED $pid"
	fi
done
for k in $KILLED; do
	for _ in $(seq 1 50); do kill -0 "$k" 2>/dev/null || break; sleep 0.1; done
	kill -0 "$k" 2>/dev/null && { echo "launch: prior nested $k survived 5s, SIGKILL"; kill -9 "$k" 2>/dev/null; sleep 0.3; }
done
sweep_nested_dirs

# --- the host, the pinned aquamarine, the private system bus ---------------
SIZE="$(sed -n 's/.*mode[[:space:]]*=[[:space:]]*"\([0-9]\+x[0-9]\+\)@.*/\1/p' "$CFG" | head -1)"
host_start "${SIZE:-1280x800}" || exit 1
HOST_WL="$(cat "$HOST_DIR/wl")"
AQ_LIB="$(aq_pinned_libdir)" || exit 1
sysbus_start || exit 1

# The bus-activated services' display: the nested's own socket, the next
# free index (the nested picks the same). A miss (a stale lock) leaves them
# a dead socket — clients fall back in-process, and everything still stays
# nested.
i=0; while [[ -e "$XDG_RUNTIME_DIR/wayland-$i.lock" || -e "$XDG_RUNTIME_DIR/wayland-$i" ]]; do i=$((i + 1)); done
NESTED_WL="wayland-$i"

before_sig="$(ls -1 "$RUNDIR" 2>/dev/null)"
echo "launch: $BIN -c $CFG (host $HOST_WL, private session + system bus)"
# setsid: a torn-down caller cannot take the instance with it.
(
	ulimit -c unlimited 2>/dev/null
	exec setsid env -u DISPLAY -u WAYLAND_SOCKET -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_ID -u SWAYSOCK \
		LIBSEAT_BACKEND=seatd LD_LIBRARY_PATH="$AQ_LIB" DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" \
		AW_BACKLIGHT_DIR="$SYSBUS_BACKLIGHT" WAYLAND_DISPLAY="$NESTED_WL" \
		dbus-run-session -- env WAYLAND_DISPLAY="$HOST_WL" "$BIN" -c "$CFG"
) >"$LOG" 2>&1 </dev/null &

# --- wait for the new instance signature and its client socket ------------
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
wl=""
for _ in $(seq 1 30); do
	wl="$(sed -n 2p "$RUNDIR/$sig/hyprland.lock" 2>/dev/null)"
	[[ -n "$wl" ]] && break
	sleep 0.1
done

printf '%s\n' "$sig" >"$HARNESS_DIR/nested.sig"
printf '%s\n' "$wl" >"$HARNESS_DIR/nested.wl"
echo "launch: nested up"
echo "  SIG=$sig  WL=$wl  HOST=$HOST_WL"
echo "  hyprctl -i $sig <cmd>   |   $(dirname "$0")/shot.sh   |   log: $(nested_log "$sig")"
