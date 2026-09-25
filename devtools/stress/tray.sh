#!/usr/bin/env bash
# The hyprbar tray battery: the StatusNotifierItem lifecycle against the bar.
# fake-sni serves one magenta icon and a dbusmenu whose root layout carries
# a doubled separator and a trailing one (sub carries a trailing one too),
# so the strip, the panel heights, and the submenu cascade all assert the
# parse-time trim (leading drop, consecutive collapse, trailing discard) in
# Menu::loadLevelNow. Expected heights with ROWH=24 SEPH=8 PAD=4:
#   root = 8 + 4*24 + 8 = 112   sub = 8 + 2*24 = 56
# The panels are glass boxes with a 1px border, sitting 1px below the bar;
# a sub panel cascades flush against the root (no gap column), so boxes are
# discriminated by their bright-column extent, not by connected components:
#   root only:  one span  root + sub:  a span roughly twice as wide
# Helpers: capture_nested (jittered), vp, chk from the harness.

SNIBIN="$REPO/devtools/fake-sni"
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
sni_items() {
	nbus get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
		org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>/dev/null
}
icon_box() { # icon_box <img> — "cx cy" of the magenta block in the bar band, "0 0" if absent
	# convert('RGB') everywhere: a mode mismatch (grayscale/palette frame)
	# would crash the index below with a confusing error instead of a 0 0.
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
w = im.size[0]
pts = [(x, y) for x in range(w // 2, w) for y in range(26)
       if px[x, y][0] > 150 and px[x, y][1] < 90 and px[x, y][2] > 150]
if len(pts) < 50:
    print("0 0"); sys.exit()
xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
print((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
PY
}
# panel_extent <img> — "width x0 x1 ytop ybot" over the menu area below the
# bar: a column counts when its min-channel>5 run below y=27 spans more than
# 40 px (the stress desktop sits <=5 by design; the bar ends at y 25). The
# sub panel cascades flush against the root, so the two panels form one
# column span; width separates the states (root ~180 px, root+sub ~360 px).
panel_extent() {
	# convert('RGB') + image-derived bounds: a mode mismatch or a capture
	# that is not the full output used to crash the metric (IndexError) and
	# take the rest of the battery down with it.
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
w, h = im.size
good = []
for x in range(w):
    run = [y for y in range(27, h) if min(px[x, y][:3]) > 5]
    if len(run) > 20 and max(run) - min(run) > 40:
        good.append((x, min(run), max(run)))
if not good:
    print("0 0 0 0 0"); sys.exit()
x0, x1 = good[0][0], good[-1][0]
yt = min(g[1] for g in good)
yb = max(g[2] for g in good)
print(x1 - x0 + 1, x0, x1, yt, yb)
PY
}
col_h() { # col_h <img> <x> — the vertical run height at one column (0 if none)
	# x is clamped: the probe derives from a span edge (+40 px) and a span
	# flush with the capture's right edge would otherwise index out of range
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x = min(int(sys.argv[2]), im.size[0] - 1)
run = [y for y in range(27, im.size[1]) if min(px[x, y][:3]) > 5]
print(max(run) - min(run) + 1 if len(run) > 20 and max(run) - min(run) > 40 else 0)
PY
}

# ---- fixture up -----------------------------------------------------------
make -C "$REPO/devtools" fake-sni >/dev/null 2>&1
setsid env DBUS_SESSION_BUS_ADDRESS="$NBUS" "$SNIBIN" >"$STATE/fake-sni.log" 2>&1 </dev/null &
FAKE_PID=$!
for _ in $(seq 1 50); do
	sni_items | grep -q "org.freedesktop.HyprFakeSNI/StatusNotifierItem" && break
	sleep 0.2
done
chk "tray: watcher accepted the fake item" test "$(sni_items | grep -c HyprFakeSNI)" = 1
sleep 1.2
capture_nested "$STATE/tray-icon.png"
chk "tray: icon capture rendered" test -s "$STATE/tray-icon.png"
ICON="$(icon_box "$STATE/tray-icon.png")"
chk "tray: magenta icon sits in the bar band" bash -c "[[ '${ICON:0:1}' != '0' ]]"
read -r IX IY <<<"$ICON"

# ---- root menu: doubled + trailing separators collapse to one -----------
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$IX" "$IY" | vp
sleep 1.5
capture_nested "$STATE/tray-root.png"
chk "tray: root menu capture rendered" test -s "$STATE/tray-root.png"
read -r RW RX0 RX1 RYT RYB <<<"$(panel_extent "$STATE/tray-root.png")"
chk "tray: one panel span open (root only)" test "$RW" -ge 120
chk "tray: one panel span open (root only, no cascade)" test "$RW" -le 260
# root panel height: the run at a mid column (the sub has not opened yet)
ROOT_H="$(col_h "$STATE/tray-root.png" "$(( (RX0 + RX1) / 2 ))")"
chk "tray: root panel height 112 (4 rows + 1 collapsed separator)" \
	test "$ROOT_H" -ge 110
chk "tray: root panel height 112 (upper bound)" \
	test "$ROOT_H" -le 116

# ---- submenu cascade ------------------------------------------------------
# rows from the top: PAD 4, one 24, two 24, SEP 8, three 24, sub 24 ->
# the sub row spans y0+80..y0+104 of the panel body. The sub panel cascades
# against the root's open side (left here): probe a column inside it.
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
	"$((RX0 + 20))" "$((RYT + 92))" | vp
sleep 1.5
capture_nested "$STATE/tray-sub.png"
chk "tray: sub menu capture rendered" test -s "$STATE/tray-sub.png"
read -r SW SX0 SX1 SYT SYB <<<"$(panel_extent "$STATE/tray-sub.png")"
chk "tray: cascade widened the span (sub panel opened)" test "$SW" -ge 290
# the sub panel occupies the side that grew: probe a column 40 px inside the
# new edge of the span (which side grew is layout-dependent)
if [[ "$SX0" -lt "$RX0" ]]; then
	SUBX=$((SX0 + 40))   # grew left
else
	SUBX=$((RX1 + 40))   # grew right
fi
SUB_H="$(col_h "$STATE/tray-sub.png" "$SUBX")"
chk "tray: sub panel height 56 (trailing separator trimmed)" \
	test "$SUB_H" -ge 54
chk "tray: sub panel height 56 (upper bound)" \
	test "$SUB_H" -le 60

# ---- outside click closes both -------------------------------------------
printf 'move 300 400\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' | vp
sleep 1.2
capture_nested "$STATE/tray-closed.png"
chk "tray: outside click closed every panel" \
	test "$(panel_extent "$STATE/tray-closed.png" | awk '{print $1}')" -le 10

# ---- teardown: the item leaves the strip ---------------------------------
kill "$FAKE_PID" 2>/dev/null
for _ in $(seq 1 30); do
	kill -0 "$FAKE_PID" 2>/dev/null || break
	sleep 0.2
done
chk "tray: watcher dropped the item" test "$(sni_items | grep -c HyprFakeSNI)" = 0
sleep 1.2
capture_nested "$STATE/tray-gone.png"
chk "tray: icon left the strip" test "$(icon_box "$STATE/tray-gone.png")" = "0 0"
