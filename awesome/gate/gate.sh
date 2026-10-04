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
usage: gate.sh [-s FORK-DIR] [-b TIER|LIST] [-k LIST] [compositor-bin]
  -s DIR    a built fork checkout (e.g. ~/repo/Hyprland): stage its build's
            header set (cmake --install to $HYPR_GATE_TMP/fork-pkg, .pc
            prefix normalized, version.h verified) as the target headers,
            and default the compositor to DIR/build/Hyprland
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
FORK_SRC=""
while [[ $# -gt 0 ]]; do
	case $1 in
	-b)  [[ $# -ge 2 ]] || { usage; exit 2; }; B_SPEC=$2; shift 2 ;;
	-b*) B_SPEC=${1#-b}; shift ;;
	-k)  [[ $# -ge 2 ]] || { usage; exit 2; }; K_SPEC=$2; shift 2 ;;
	-k*) K_SPEC=${1#-k}; shift ;;
	-s)  [[ $# -ge 2 ]] || { usage; exit 2; }; FORK_SRC=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	-*) usage; exit 2 ;;
	*)  if [[ -n "$BIN" ]]; then
		echo "gate.sh: unexpected argument '$1'" >&2; usage; exit 2
		fi
		BIN=$1; shift ;;
	esac
done
# -s: stage the fork build's header set ourselves — one command instead of
# a hand-run cmake --install, a .pc prefix fix, and two env vars. CMake
# writes the CONFIGURED prefix into hyprland.pc; the harness normalizes a
# copy (normalize_target_pkgconfig). version.h is generated at CONFIGURE
# time only: a build tree configured at another commit carries a stale
# hash, which the plugin's load guard then rejects — refuse it here.
if [[ -n "$FORK_SRC" ]]; then
	FORK_SRC="$(cd "$FORK_SRC" 2>/dev/null && pwd)" || { echo "gate.sh: no such fork dir" >&2; exit 2; }
	[[ -x "$FORK_SRC/build/Hyprland" ]] || { echo "gate.sh: $FORK_SRC/build/Hyprland is not built" >&2; exit 2; }
	_head="$(git -C "$FORK_SRC" rev-parse HEAD 2>/dev/null)"
	grep -q "GIT_COMMIT_HASH *\"$_head\"" "$FORK_SRC/src/version.h" 2>/dev/null || {
		echo "gate.sh: $FORK_SRC/src/version.h is not HEAD ($_head): reconfigure (cmake -S . -B build) and rebuild" >&2
		exit 2
	}
	_pkg="${HYPR_GATE_TMP:-/tmp/hypr-gate}/fork-pkg"
	rm -rf -- "$_pkg"
	# install_manifest.txt in a root-owned build dir fails the exit code
	# after every file is in place: judge the result, not the rc
	cmake --install "$FORK_SRC/build" --prefix "$_pkg" >/dev/null 2>&1
	cmp -s "$FORK_SRC/src/version.h" "$_pkg/include/hyprland/src/version.h" && [[ -f "$_pkg/share/pkgconfig/hyprland.pc" ]] || {
		echo "gate.sh: staging the fork headers into $_pkg failed" >&2
		exit 2
	}
	# the staged set is ours: point its .pc at itself, so a manual
	# `PKG_CONFIG_PATH=$_pkg/share/pkgconfig make` builds against it too
	sed -i "s|^prefix=.*|prefix=$_pkg/include|" "$_pkg/share/pkgconfig/hyprland.pc"
	export PKG_CONFIG_PATH="$_pkg/share/pkgconfig" HYPR_DEPLOY_PKG_CONFIG_PATH="$_pkg/share/pkgconfig"
	[[ -n "$BIN" ]] || BIN="$FORK_SRC/build/Hyprland"
	unset _head _pkg
fi
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

# Fallback summary for runs without lifecycle.sh. Teardown first, like
# lifecycle.sh's tail: the isolation verdict and the nested's exit check
# land in the summary and the exit code, not after them
# (cleanup_harness is idempotent via HARNESS_CLEANED; the EXIT trap is the
# backstop).
cleanup_harness
print_summary
exit $?
