#!/usr/bin/env bash
# awesome/gate/stop.sh — tear the nested dev environment down: the nested
# instance (any harness-style launch, never the live compositor), its
# private host and system bus, and the dead instances' runtime dirs. The
# live session was never part of it, so there is nothing to restore there.
#
# The gate's own teardown (harness.sh) runs the same steps on its exit path;
# this is the standalone version for a manual launch.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
# shellcheck source=live.sh
. "$HERE/live.sh" # RUNDIR, LIVE_PID (read-only), is_nested_pid, sweep_nested_dirs
# shellcheck source=host.sh
. "$HERE/host.sh" # host_stop, sysbus_stop

KILLED=""
for pid in $(pgrep -x Hyprland 2>/dev/null); do
	if is_nested_pid "$pid"; then
		echo "stop: killing nested pid $pid"
		kill "$pid" 2>/dev/null
		KILLED="$KILLED $pid"
	fi
done
for k in $KILLED; do
	for _ in $(seq 1 50); do kill -0 "$k" 2>/dev/null || break; sleep 0.1; done
	kill -0 "$k" 2>/dev/null && kill -9 "$k" 2>/dev/null
done
host_stop
sysbus_stop
sweep_nested_dirs
rm -f "$HARNESS_DIR/nested.sig" "$HARNESS_DIR/nested.wl"
echo "stop: done"
