#!/usr/bin/env bash
# Bootstrap for isolated probes against the stress target. Source this file,
# then either `retarget` (a stress-config nested is already running) or
# `launch_stress_nested` (kill whatever is running, relaunch fresh, wait out
# the no-watchdog toast). After that, `source notify-lib.sh` for the shared
# notification helpers.
#
#   source "$REPO/devtools/stress/probe-env.sh"
#   launch_stress_nested || exit 1
#   source "$REPO/devtools/stress/notify-lib.sh"
#
# HYPR_BIN selects the compositor (default: the fork build tree); the caller
# must export PKG_CONFIG_PATH / HYPR_DEPLOY_PKG_CONFIG_PATH only when it also
# rebuilds plugins — a probe against built .so files needs neither.

REPO="${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
STRESS_DIR="$REPO/devtools/stress"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
BIN="${HYPR_BIN:-$HOME/repo/Hyprland/build/Hyprland}"
STATE="$HARNESS/stress-state"
CFG="$HARNESS/stress.lua"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""; WL=""; MON_W=0; MON_H=0; NBUS=""
CAPTURE_PID=""; CLIP_PID=""; PKG_COPY_DIR=""
HARNESS_CLEANED=0; HARNESS_OUTPUT_OWNED=0
PASS=0; FAILED=()

# shellcheck source=devtools/stress/harness.sh
source "$STRESS_DIR/harness.sh"

# A probe owns its nested: without this trap a failed probe leaks the
# compositor and the next probe's kill_nested becomes the only cleanup.
trap cleanup_harness EXIT

launch_stress_nested() { # kill, relaunch fresh, validate, wait out the toast
	kill_nested
	fresh_stress_state
	write_stress_cfg
	launch_nested || { echo "probe: nested launch FAILED" >&2; return 1; }
	retarget || { echo "probe: nested retarget FAILED" >&2; return 1; }
	wait_launch_toast || return 1
	echo "probe: nested on $WL at ${MON_W}x${MON_H} ($(hq plugin list | grep -c Plugin) plugins)"
}
