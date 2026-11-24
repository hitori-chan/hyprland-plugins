#!/usr/bin/env bash
# The awesome plugin's integration gate.
#
# The plugin's behavioral contract: the scenario batteries run in this
# shell and share one validated nested target and fixture state.
# The operational core is harness.sh (launch/teardown/retarget/capture/
# coredumps + the monolith's store fixtures and config writer); the input
# fixtures it drives (vptr, vkbd, cliphold, ...) are the shared devtools/.
set -u

source "$(dirname "${BASH_SOURCE[0]}")/memcap.sh" # the run's memory cap: before anything builds or launches
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export GATE_DIR
# nested.lua resolves the .so through this (falls back to a HOME path; the
# preflight builds from $REPO, so the nested must load the same tree)
export HYPR_PLUGIN_DIR="$REPO"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
# One run at a time: every entry point shares $HARNESS (the nested's
# signature, the host, the system bus) and kills every nested it finds —
# a second run would tear the first one's nested down mid-battery. The
# lock fd is closed for the long-lived children (the nested, the host).
if [[ -z "${HYPR_HARNESS_LOCKED:-}" ]]; then
	mkdir -p "$HARNESS"
	exec 9>"$HARNESS/.lock"
	flock -n 9 || { echo "$(basename "$0"): another gate/probe/dev run holds $HARNESS/.lock" >&2; exit 2; }
	export HYPR_HARNESS_LOCKED=1
fi

# Battery selection: resolved against this canonical order; sourcing below
# always walks it so user-specified order never changes execution order.
#
# Tiers: the dev loop runs QUICK (a smoke of each module) by default; `all`
# runs every module battery (quick is a subset of them).
CANONICAL_BATTERIES=(quick shell windows notify system pipeline lifecycle)
QUICK_TIER=(quick)
ALL_TIER=(shell windows notify system pipeline lifecycle)
SELECTED=()

usage() {
	cat >&2 <<'EOF'
usage: gate.sh [-s FORK-DIR] [-b TIER|LIST] [-k LIST] [-p TRACE] [compositor-bin]
  -s DIR    a built fork checkout (e.g. ~/repo/Hyprland): stage its build's
            header set (cmake --install to $HYPR_GATE_TMP/fork-pkg, .pc
            prefix normalized, version.h verified, the pinned aquamarine's
            .pc beside it) as the target headers, and default the
            compositor to DIR/build/Hyprland
  -b        tiers:
              quick      the dev-loop smoke battery (DEFAULT)
              all        shell windows notify system pipeline lifecycle,
                         and the release build's deploy rehearsal
                         (REHEARSE=1 adds it to any tier)
            or a comma-separated battery list, from:
              quick shell windows notify system pipeline lifecycle
            (canonical order enforced regardless of user order)
  -k LIST   comma-separated batteries to SKIP from the selected set
  -p TRACE  trace the batteries' wall time into TRACE; gate/profile.py
            TRACE names the lines it went to
EOF
}

is_canonical() {
	local name
	for name in "${CANONICAL_BATTERIES[@]}"; do
		[[ "$name" == "$1" ]] && return 0
	done
	return 1
}

is_selected() {
	local name
	for name in "${SELECTED[@]}"; do
		[[ "$name" == "$1" ]] && return 0
	done
	return 1
}

die_unknown() {
	echo "gate.sh: unknown battery '$1'" >&2
	usage
	exit 2
}

# Manual parsing (not getopts): getopts stops at the first positional, so
# 'gate.sh BIN -b shell' would silently run the full gate.
B_SPEC=""
K_SPEC=""
PROFILE=""
BIN=""
FORK_SRC=""
while [[ $# -gt 0 ]]; do
	case $1 in
	-b)  [[ $# -ge 2 ]] || { usage; exit 2; }; B_SPEC=$2; shift 2 ;;
	-b*) B_SPEC=${1#-b}; shift ;;
	-k)  [[ $# -ge 2 ]] || { usage; exit 2; }; K_SPEC=$2; shift 2 ;;
	-k*) K_SPEC=${1#-k}; shift ;;
	-p)  [[ $# -ge 2 ]] || { usage; exit 2; }; PROFILE=$2; shift 2 ;;
	-s)  [[ $# -ge 2 ]] || { usage; exit 2; }; FORK_SRC=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	-*) usage; exit 2 ;;
	*)  if [[ -n "$BIN" ]]; then
		echo "gate.sh: unexpected argument '$1'" >&2; usage; exit 2
		fi
		BIN=$1; shift ;;
	esac
done

if [[ -z "$B_SPEC" || "$B_SPEC" == "quick" ]]; then
	SELECTED=("${QUICK_TIER[@]}")
elif [[ "$B_SPEC" == "all" ]]; then
	SELECTED=("${ALL_TIER[@]}")
	export REHEARSE=1
else
	IFS=',' read -r -a REQUESTED <<< "$B_SPEC"
	for _name in "${REQUESTED[@]}"; do
		is_canonical "$_name" || die_unknown "$_name"
		is_selected "$_name" || SELECTED+=("$_name")
	done
fi
if [[ -n "$K_SPEC" ]]; then
	IFS=',' read -r -a SKIPPED <<< "$K_SPEC"
	for _name in "${SKIPPED[@]}"; do
		is_canonical "$_name" || die_unknown "$_name"
	done
	_KEPT=()
	for _name in "${SELECTED[@]}"; do
		_skip=false
		for _s in "${SKIPPED[@]}"; do [[ "$_name" == "$_s" ]] && _skip=true; done
		$_skip || _KEPT+=("$_name")
	done
	SELECTED=("${_KEPT[@]}")
fi
[[ ${#SELECTED[@]} -gt 0 ]] || { echo "gate.sh: no battery left to run (-b/-k selected nothing)" >&2; exit 2; }
# scratch state on tmpfs: ~45 captures a run, never worth a disk write
STATE="${HYPR_GATE_TMP:-/tmp/hypr-gate}/stress-state"
CFG="$HARNESS/stress.lua"
CAPTURE_LOG="$HARNESS/input-capture.log"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""
CAPTURE_PID=""
CLIP_PID=""
SHARE_PID=""
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
[[ -z "$FORK_SRC" ]] || stage_fork "$FORK_SRC" || exit 2
[[ -n "$BIN" ]] || BIN=${HYPR_BIN:-/usr/local/bin/Hyprland}
# Scenario modules execute their checks immediately. Install the cleanup
# owner before any module can create a compositor or fixture.
trap cleanup_harness EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# shellcheck source=awesome/gate/preflight.sh
source "$GATE_DIR/preflight.sh"
# Batteries execute on source. When lifecycle.sh is among the selected, its
# tail prints the final summary, calls cleanup_harness, and exits — ending
# this script here; the fallback summary below is then unreachable.
# -p: every main-shell line with its time and call stack (profile.py)
if [[ -n "$PROFILE" ]]; then
	exec 6>"$PROFILE"
	BASH_XTRACEFD=6
	PS4=$'+${BASH_SUBSHELL}\t${EPOCHREALTIME}\t${BASH_SOURCE[*]}\t${LINENO} ${BASH_LINENO[*]}\t'
	set -x
fi
for _name in "${CANONICAL_BATTERIES[@]}"; do
	is_selected "$_name" || continue
	battery_begin "$_name"
	source "$GATE_DIR/$_name.sh"
	# lifecycle.sh's tail (when selected) records its own battery_end and
	# exits before these lines run; for every other battery they apply here.
	assert_desktop_clean "$_name"
	battery_end "$_name"
done

# Fallback summary for runs without lifecycle.sh. Teardown first, like
# lifecycle.sh's tail: the isolation verdict and the nested's exit check
# land in the summary and the exit code, not after them
# (cleanup_harness is idempotent via HARNESS_CLEANED; the EXIT trap is the
# backstop).
cleanup_harness
print_summary
exit $?
