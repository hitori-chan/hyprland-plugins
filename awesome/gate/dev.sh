#!/usr/bin/env bash
# awesome/gate/dev.sh — one iteration of the nested dev loop against the
# monolith: stop the nested (its .so must not be mapped while it rebuilds),
# build the plugin, relaunch the nested loading that build, spawn a probe
# window so the strip has content, screenshot. Edit code, run dev.sh, look
# at the -bar.png. No hyprpm, no relog.
#
# For scripted scenarios (state checks, input, assertions) use probe.sh
# instead — it runs the gate's full preflight and executes a probe body.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

bash "$HERE/stop.sh" >/dev/null 2>&1
make -C "$REPO/awesome" >/dev/null || { echo "dev: build failed, not launching" >&2; exit 1; }
bash "$HERE/launch.sh" || exit 1

SIG="$(cat "${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}/nested.sig")"
# a probe window so the taglist/tasklist have content (nested uses Lua dispatch)
hyprctl -i "$SIG" dispatch "hl.dsp.exec_cmd('foot')" >/dev/null 2>&1
sleep 1.5
bash "$HERE/shot.sh"
