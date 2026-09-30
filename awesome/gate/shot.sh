#!/usr/bin/env bash
# awesome/gate/shot.sh [out.png] — screenshot the running nested instance and
# emit a 2x crop of the top bar strip (<out>-bar.png). Reads the nested socket
# recorded by launch.sh (falls back to hyprland.lock line 2). All calls
# time-bounded so a dead instance fails fast instead of hanging.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_DIR="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
OUT="${1:-$HARNESS_DIR/shot.png}"
SIG="$(cat "$HARNESS_DIR/nested.sig" 2>/dev/null)"
[[ -z "$SIG" ]] && { echo "shot: no nested.sig — launch first" >&2; exit 1; }

WL="$(cat "$HARNESS_DIR/nested.wl" 2>/dev/null)"
[[ -z "$WL" ]] && WL="$(sed -n 2p "$XDG_RUNTIME_DIR/hypr/$SIG/hyprland.lock" 2>/dev/null)"
[[ -z "$WL" ]] && { echo "shot: no nested Wayland socket — is it alive?" >&2; exit 1; }

if ! timeout 8 env WAYLAND_DISPLAY="$WL" grim "$OUT" 2>/dev/null; then
	echo "shot: grim failed/timed out on $WL (instance dead or frame-starved?)" >&2
	exit 1
fi
# the bar's strip (logical 26px = the C++ default; BAR_H to override)
BAR_H="${BAR_H:-26}"
BAR="${OUT%.png}-bar.png"
magick "$OUT" -crop "x${BAR_H}+0+0" +repage -scale 200% "$BAR" 2>/dev/null
echo "shot: $OUT ($(magick identify -format '%wx%h' "$OUT" 2>/dev/null))  bar: $BAR"
