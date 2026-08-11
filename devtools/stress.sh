#!/usr/bin/env bash
# Exact-fork pre-deploy gate. Scenario modules execute in this shell and share
# the validated nested target and fixture state.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS_DIR="$REPO/devtools/stress"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
BIN="${1:-${HYPR_BIN:-/usr/local/bin/Hyprland}}"
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

# shellcheck source=devtools/stress/harness.sh
source "$STRESS_DIR/harness.sh"
# shellcheck source=devtools/stress/preflight.sh
source "$STRESS_DIR/preflight.sh"
# shellcheck source=devtools/stress/windows.sh
source "$STRESS_DIR/windows.sh"
# shellcheck source=devtools/stress/notifications.sh
source "$STRESS_DIR/notifications.sh"
# shellcheck source=devtools/stress/osd-reply.sh
source "$STRESS_DIR/osd-reply.sh"
# shellcheck source=devtools/stress/policy.sh
source "$STRESS_DIR/policy.sh"
# shellcheck source=devtools/stress/lifecycle.sh
source "$STRESS_DIR/lifecycle.sh"
