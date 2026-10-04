#!/usr/bin/env bash
# awesome/gate/gate.sh — the awesome plugin's integration gate.
#
# The plugin's behavioral contract: the scenario batteries run in this
# shell and share one validated nested target and fixture state.
# manifest.tsv maps each check to its predecessor in the retired gate.
# The operational core is harness.sh (launch/teardown/retarget/capture/
# coredumps + the monolith's store fixtures and config writer); the input
# fixtures it drives (vptr, vkbd, cliphold, ...) are the shared devtools/.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export GATE_DIR
# nested.lua resolves the .so through this (falls back to a HOME path; the
# preflight builds from $REPO, so the nested must load the same tree)
export HYPR_PLUGIN_DIR="$REPO"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"

# Battery selection: resolved against this canonical order; sourcing below
# always walks it so user-specified order never changes execution order.
#
# Tiers (2026-10-04 gate trim): the dev loop runs QUICK by default; the
# battle-tested batteries that pay for relaunches (state) or X-wayland
# pipeline polling (focus) are opt-in, and `all` is the pre-trim full run
# without focus. `everything` runs every battery.
CANONICAL_BATTERIES=(quick shell windows state notify system pipeline focus lifecycle)
QUICK_TIER=(quick)
ALL_TIER=(shell windows state notify system pipeline lifecycle)
SELECTED=()

usage() {
	cat >&2 <<'EOF'
usage: gate.sh [-b TIER|LIST] [-k LIST] [compositor-bin]
  -b        tiers:
              quick      the dev-loop smoke battery (DEFAULT)
              all        shell windows state notify system pipeline lifecycle
              everything quick + all + focus
            or a comma-separated battery list, from:
              quick shell windows state notify system pipeline focus lifecycle
            (canonical order enforced regardless of user order)
  -k LIST   comma-separated batteries to SKIP from the selected set
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
BIN=""
while [[ $# -gt 0 ]]; do
	case $1 in
	-b)  [[ $# -ge 2 ]] || { usage; exit 2; }; B_SPEC=$2; shift 2 ;;
	-b*) B_SPEC=${1#-b}; shift ;;
	-k)  [[ $# -ge 2 ]] || { usage; exit 2; }; K_SPEC=$2; shift 2 ;;
	-k*) K_SPEC=${1#-k}; shift ;;
	-h|--help) usage; exit 0 ;;
	-*) usage; exit 2 ;;
	*)  if [[ -n "$BIN" ]]; then
		echo "gate.sh: unexpected argument '$1'" >&2; usage; exit 2
		fi
		BIN=$1; shift ;;
	esac
done
[[ -n "$BIN" ]] || BIN=${HYPR_BIN:-/usr/local/bin/Hyprland}

if [[ -z "$B_SPEC" || "$B_SPEC" == "quick" ]]; then
	SELECTED=("${QUICK_TIER[@]}")
elif [[ "$B_SPEC" == "all" ]]; then
	SELECTED=("${ALL_TIER[@]}")
elif [[ "$B_SPEC" == "everything" ]]; then
	SELECTED=("${CANONICAL_BATTERIES[@]}")
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
STATE="$HARNESS/stress-state"
CFG="$HARNESS/stress.lua"
CAPTURE_LOG="$HARNESS/input-capture.log"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""
CAPTURE_PID=""
CLIP_PID=""
PASS=0
FAILED=()
WL=""
MON_W=0
MON_H=0
NBUS=""
PKG_COPY_DIR=""
HARNESS_CLEANED=0
HARNESS_OUTPUT_OWNED=""

# shellcheck source=awesome/gate/harness.sh
source "$GATE_DIR/harness.sh"
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
for _name in "${CANONICAL_BATTERIES[@]}"; do
	is_selected "$_name" || continue
	battery_begin "$_name"
	source "$GATE_DIR/$_name.sh"
	# lifecycle.sh's tail (when selected) records its own battery_end and
	# exits before these lines run; for every other battery they apply here.
	assert_desktop_clean "$_name"
	battery_end "$_name"
done

# Fallback summary for runs without lifecycle.sh. Cleanup happens in the EXIT
# trap either way (cleanup_harness is idempotent via HARNESS_CLEANED).
print_summary
exit $?
