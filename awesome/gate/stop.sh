#!/usr/bin/env bash
# awesome/gate/stop.sh — tear the nested dev environment down cleanly: kill
# any nested instance launched from this config and remove the off-screen
# virtual monitor. Leaves the live session exactly as it was.
#
# The gate's own teardown (harness.sh) does the same on its exit path; this
# is the standalone version for a manual launch.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
CFG="${HYPR_CFG:-$HERE/nested.lua}"
VM="nested-dev"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"

for s in "$RUNDIR"/*/; do
	sig="$(basename "$s")"
	[[ "$sig" == "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] && continue
	pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
	[[ -n "$pid" ]] || continue
	# Match any nested-style launch (a -c config token, never the live
	# --watchdog-fd session), not just this $CFG: an orphan from another
	# cfg (manual/old) outlived every stop and its window took the live
	# focus once nested-dev went away (2026-10-02, /tmp debug cfg).
	if grep -qa -- "$CFG" "/proc/$pid/cmdline" 2>/dev/null \
		|| { grep -Fzxq -- "-c" "/proc/$pid/cmdline" 2>/dev/null && ! grep -Fzxq -- "--watchdog-fd" "/proc/$pid/cmdline" 2>/dev/null; }; then
		echo "stop: killing nested $sig (pid $pid)"
		kill "$pid" 2>/dev/null
	fi
done
sleep 0.5

if hyprctl monitors -j 2>/dev/null | python3 -c "import json,sys;ms=json.load(sys.stdin);sys.exit(0 if any(m['name']=='$VM' for m in ms) else 1)"; then
	echo "stop: removing virtual monitor $VM"
	hyprctl output remove "$VM" >/dev/null 2>&1
fi
rm -f "$HARNESS_DIR/nested.sig" "$HARNESS_DIR/nested.wl" "$HARNESS_DIR/nested.output-owned"
echo "stop: done"
