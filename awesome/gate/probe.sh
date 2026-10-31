#!/usr/bin/env bash
# Isolated one-off probes against a live nested.
#
# Runs the gate's full preflight (build, launch, retarget — the SAME
# machinery the batteries get, so a probe result is gate-valid), then
# executes the probe script passed as $1 with the harness and lib
# libraries already sourced: hq/dsp/st/bd, capture_nested, the fixture
# drivers, SIG/WL/STATE, ok/bad/chk. Teardown ALWAYS runs on exit.
#
# Usage:
#   probe.sh /path/to/probe-body.sh          # installed target
#   probe.sh -b /path/to/bin /path/probe     # explicit compositor bin
#
# A probe body is a shell fragment, executed top to bottom; ok/bad
# count into the printed summary (which reports PASS/FAILED like a
# battery). Anything that must survive is copied out of $STATE before
# the exit trap (it is on tmpfs, wiped by fresh_stress_state on the next run).

set -u

source "$(dirname "${BASH_SOURCE[0]}")/memcap.sh" # the run's memory cap: before anything builds or launches
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export GATE_DIR
export HYPR_PLUGIN_DIR="$REPO"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
# One run at a time (see gate.sh).
if [[ -z "${HYPR_HARNESS_LOCKED:-}" ]]; then
	mkdir -p "$HARNESS"
	exec 9>"$HARNESS/.lock"
	flock -n 9 || { echo "$(basename "$0"): another gate/probe/dev run holds $HARNESS/.lock" >&2; exit 2; }
	export HYPR_HARNESS_LOCKED=1
fi

BIN=""
PROBE=""
while [[ $# -gt 0 ]]; do
	case $1 in
	-b)  [[ $# -ge 2 ]] || { echo "probe.sh: -b needs a path" >&2; exit 2; }; BIN=$2; shift 2 ;;
	-b*) BIN=${1#-b}; shift ;;
	-h|--help)
		echo "usage: probe.sh [-b compositor-bin] probe-body.sh"
		exit 0 ;;
	-*)  echo "probe.sh: unknown flag '$1'" >&2; exit 2 ;;
	*)   PROBE=$1; shift ;;
	esac
done
[[ -n "$PROBE" ]] || { echo "probe.sh: no probe body given" >&2; exit 2; }
[[ -f "$PROBE" ]] || { echo "probe.sh: no such probe: $PROBE" >&2; exit 2; }
[[ -n "$BIN" ]] || BIN=${HYPR_BIN:-/usr/local/bin/Hyprland}

STATE="${HYPR_GATE_TMP:-/tmp/hypr-gate}/stress-state" # tmpfs
CFG="$HARNESS/stress.lua"
CAPTURE_LOG="$HARNESS/input-capture.log"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""
CAPTURE_PID=""
CLIP_PID=""
PASS=0
FAILED=()
SKIPS=()
WL=""
MON_W=0
MON_H=0
NBUS=""
PKG_COPY_DIR=""
HARNESS_CLEANED=0

# shellcheck source=awesome/gate/harness.sh
source "$GATE_DIR/harness.sh"
trap cleanup_harness EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "== probe: $(basename "$PROBE") =="
# shellcheck source=awesome/gate/preflight.sh
source "$GATE_DIR/preflight.sh"
# lib.sh AFTER the preflight: its geometry derives from the monitor size
# the launch measured (sourced before, every coordinate would derive from
# MON_W=0)
# shellcheck source=awesome/gate/lib.sh
source "$GATE_DIR/lib.sh"
source "$PROBE"
cleanup_harness
print_summary
exit $?
