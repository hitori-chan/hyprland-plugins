#!/usr/bin/env bash
# Exact-fork pre-deploy gate. Scenario modules execute in this shell and share
# the validated nested target and fixture state.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS_DIR="$REPO/devtools/stress"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"

# Battery selection: resolved against this canonical order; sourcing below
# always walks it so user-specified order never changes execution order.
CANONICAL_BATTERIES=(windows notifications reply policy lifecycle)
SELECTED=()

usage() {
	cat >&2 <<'EOF'
usage: stress.sh [-b LIST] [-k LIST] [compositor-bin]
  -b LIST   comma-separated batteries to RUN, from:
            windows notifications reply policy lifecycle (canonical order enforced regardless of user order)
            special value: all (default)
  -k LIST   comma-separated batteries to SKIP from the canonical set
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
	echo "stress.sh: unknown battery '$1'" >&2
	usage
	exit 2
}

B_SPEC=""
K_SPEC=""
while getopts ":b:k:" opt; do
	case "$opt" in
	b) B_SPEC="$OPTARG" ;;
	k) K_SPEC="$OPTARG" ;;
	\?|:) usage; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
BIN="${1:-${HYPR_BIN:-/usr/local/bin/Hyprland}}"

if [[ -z "$B_SPEC" || "$B_SPEC" == "all" ]]; then
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

# shellcheck source=devtools/stress/harness.sh
source "$STRESS_DIR/harness.sh"
# Scenario modules execute their checks immediately. Install the cleanup
# owner before any module can create a compositor or fixture.
trap cleanup_harness EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# shellcheck source=devtools/stress/preflight.sh
source "$STRESS_DIR/preflight.sh"
# Batteries execute on source. When lifecycle.sh is among the selected, its
# tail prints the final summary, calls cleanup_harness, and exits — ending this
# script here; the fallback summary below is then unreachable. Batteries are
# otherwise independent: each sources notify-lib.sh only when it needs the
# notification helpers, so any subset can run alone.
for _name in "${CANONICAL_BATTERIES[@]}"; do
	is_selected "$_name" || continue
	echo
	echo "== battery: $_name ($SECONDS s in) =="
	source "$STRESS_DIR/$_name.sh"
done

# Fallback summary for runs without lifecycle.sh. Cleanup happens in the EXIT
# trap either way (cleanup_harness is idempotent via HARNESS_CLEANED).
echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
	echo "== stress: ALL $PASS CHECKS PASSED in ${SECONDS}s =="
	exit 0
else
	echo "== stress: $PASS passed, ${#FAILED[@]} FAILED in ${SECONDS}s =="
	printf '   - %s\n' "${FAILED[@]}"
	exit 1
fi
