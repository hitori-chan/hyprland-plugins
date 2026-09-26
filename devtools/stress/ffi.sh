# ffi — the Rust "awesome" probe (cabi C ABI). It is loaded for the WHOLE run
# (nested.lua lists it last, so it never reorders the C++ plugins' input
# priority table) and is the first non-C++ code to cross the fork's C boundary.
#
# What this battery asserts:
#   1. The loader took the C path — "awesome" is in the plugin list with the
#      metadata the probe wrote through the out-params (name/version/desc are
#      C strings the fork copied; a wrong ABI or a broken handshake would drop
#      the plugin or mangle them).
#   2. The probe survives config reloads. A reload re-applies the config while
#      the probe's dispatchers/jobs stay armed; a dangling callback into the
#      plugin (or a job firing mid-reload) would crash the nested or drop the
#      plugin from the list.
# The clean-unload side (hyprPluginExitC cancels jobs + clears listeners before
# the ctx is dropped) is covered by the lifecycle battery's teardown core check.

# 1. present + metadata round-trip -----------------------------------------
chk "ffi: Rust probe is in the plugin list" test "$(hq plugin list | grep -c 'Plugin awesome')" = 1
chk "ffi: Rust probe reports its C-ABI version (1.0.0)" \
    test "$(hq plugin list | grep -A3 'Plugin awesome' | grep -c 'Version: 1.0.0')" = 1
chk "ffi: Rust probe metadata round-trips (cabi description)" \
    test "$(hq plugin list | grep -A3 'Plugin awesome' | grep -c 'cabi')" = 1

# 2. survive config reloads -------------------------------------------------
for i in 1 2 3; do
	chk "ffi: config reload #$i re-applies" hq_matches 'ok' reload
	sleep 1.5
	chk "ffi: Rust probe survives config reload #$i" \
	    test "$(hq plugin list | grep -c 'Plugin awesome')" = 1
	chk "ffi: all $NLOADED plugins alive after reload #$i" \
	    test "$(hq plugin list | grep -c Plugin)" = "$NLOADED"
done

# 3. The Phase 1 mini-bar renders (canvas + textures) -------------------------
# The Rust mini-bar paints the BOTTOM strip of the monitor (the C++ hyprbar owns
# the top, and the Rust plugin loads last so it never reorders input priority).
# A capture's bottom strip must show the bar's dark blue-gray content (not
# black) — proving the cabi render pipeline (canvas glass/border/rect, the RGBA
# icon, the text clock, and damage) works end to end in the nested.
_bar_ok=0
if capture_nested "$STATE/ffi-bar.png" && python3 - "$STATE/ffi-bar.png" "$MON_W" "$MON_H" >/dev/null 2>&1 <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
w, h = int(sys.argv[2]), int(sys.argv[3])
# bottom strip (the Rust mini-bar): full width, the bottom ~20 rows
vals = [px[x, y] for y in range(h - 20, h) for x in range(0, w, 8)]
avg = tuple(sum(c[i] for c in vals) / len(vals) for i in range(3))
# the strip is a dark blue-gray (~25,34,47, sum ~106); black would be 0
sys.exit(0 if sum(avg) > 40 else 1)
PY
then
	_bar_ok=1
fi
if [[ "$_bar_ok" == 1 ]]; then ok "ffi: Rust mini-bar paints the bottom strip"; else bad "ffi: Rust mini-bar paints the bottom strip"; fi
