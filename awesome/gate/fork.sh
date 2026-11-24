#!/usr/bin/env bash
# fork.sh [FORK-DIR] — build the fork (default ~/repo/Hyprland) as the gate
# runs it, then its unit tests: in the run's memory cap with its job
# budget and ccache (memcap.sh), against the pinned Aquamarine with the
# package's patches (host.sh), reconfigured first so version.h names HEAD
# and pkg-config's cached dependency versions are current.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/memcap.sh" # the run's memory cap: before anything builds
GATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FORK_SRC="$(cd "${1:-$HOME/repo/Hyprland}" 2>/dev/null && pwd)" || { echo "fork.sh: no such fork dir" >&2; exit 2; }
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
. "$GATE_DIR/host.sh"
AQ="$(aq_pinned_libdir)" || exit 1
# CPATH as well: a unit built without aquamarine's -I must not fall back to
# the system's unpatched headers (another COutputState layout, no error)
export PKG_CONFIG_PATH="$AQ/pkgconfig" CPATH="${AQ%/lib}/include"
cd "$FORK_SRC" || exit 2
# the tests by name, not by a build dir's cache; coverage (upstream's default
# with tests) would instrument the compositor the gate runs
cmake -S . -B build -DCMAKE_CXX_COMPILER_LAUNCHER="$CCACHE" -DCMAKE_C_COMPILER_LAUNCHER="$CCACHE" -DBUILD_TESTING=ON -DWITH_COVERAGE=OFF \
	-U '*_dep_*' -U '__pkg_config_checked_*' > /dev/null || exit 1
cmake --build build -j"$HYPR_HARNESS_JOBS" || exit 1
cmake --build build -j"$HYPR_HARNESS_JOBS" --target hyprland_gtests > /dev/null || exit 1
./build/hyprland_gtests --gtest_brief=1
