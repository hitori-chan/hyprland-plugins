# awesome/gate/shell.sh — the shell module's behavior battery: the strip's
# presence and state (tags, tasks, the bell badge, the clock), the tray
# lifecycle against the bar, and the menubar/launcher. Helpers:
# capture_nested, vp, vk, chk, hq, ws, dsp from the harness; strip_band,
# bar_bright, bell_x from lib.sh.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- the strip -------------------------------------------------------------
# The strip warms in over its first frames after launch (the warm/draw gate
# never paints in the frame that creates a texture); a capture that lands
# mid-warm counts the absent glyphs. Wait for the strip to actually draw —
# bounded, so a strip that never draws still fails the check below.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if capture_nested "$STATE/strip-base.png" && [[ "$(bar_bright "$STATE/strip-base.png")" -gt 300 ]]; then
		break
	fi
	sleep 0.5
done
# one reading for the log either way: a failed canary must say HOW dark
# the strip was (0 = never drew; 150-300 = mid-warm), not just "dark"
echo "shell: strip bright=$(bar_bright "$STATE/strip-base.png")" >&2
chk "shell: the strip is on screen (clock + tags draw)" \
	test "$(bar_bright "$STATE/strip-base.png")" -gt 300
BELL0="$(bell_x "$STATE/strip-base.png")"
chk "shell: the bell glyph is the leftmost bright run of the right cluster" test "$BELL0" -gt 400

# ---- tags: the click switches the viewed workspace -------------------------
# The tag cells are glyph + 12px padding, so their centers are not a fixed
# pitch: the Nth tag's cell is found by its glyph's bright run in the
# strip's left half (the first 9 runs are the nine kanji).
tag_x() { # tag_x <frame> <n> — the n-th tag's glyph center x (0 if absent)
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
w = im.size[0]
n = int(sys.argv[2])
runs, inrun = [], False
for x in range(0, w // 2):
    bright = any(min(px[x, y][:3]) > 120 for y in range(4, 23))
    if bright and not inrun:
        start = x; inrun = True
    elif not bright and inrun:
        runs.append((start, x - 1)); inrun = False
if inrun:
    runs.append((start, w // 2 - 1))
if len(runs) < n:
    print(0); sys.exit()
a, b = runs[n - 1]
print((a + b) // 2)
PY
}
chk "shell: starts on workspace 1" test "$(ws)" = 1
TAG3_X="$(tag_x "$STATE/strip-base.png" 3)"
chk "shell: tag 3's glyph is found in the strip" test "$TAG3_X" -gt 0
printf 'move %s 13\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$TAG3_X" | vp
sleep 1.2
chk "shell: clicking tag 三 switches to workspace 3" test "$(ws)" = 3
capture_nested "$STATE/strip-ws3.png"
chk "shell: the viewed tag carries the focus background" \
	python3 - "$STATE/strip-ws3.png" "$TAG3_X" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
cx = int(sys.argv[2])
# tag 3's cell around its glyph: col_active_bg (0x339acbff) over col_bg
# (0xff132732) composites to ~(46,72,91) — a slate the bar's own background
# (19,39,50) and the bright text (238,243,245) are both far from, so the hue
# isolates the fill.
n = 0
for y in range(4, 22):
    for x in range(cx - 14, cx + 15):
        r, g, b = px[x, y]
        if r < 60 and g > 55 and b > 75 and b > g:
            n += 1
sys.exit(0 if n > 100 else 1)
PY
dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 0.8
chk "shell: back on workspace 1" test "$(ws)" = 1

# ---- tasks: the chip appears with the window, dims with minimize -----------
TASKBAND0="$(strip_band "$STATE/strip-base.png" 250 600 100)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
capture_nested "$STATE/strip-task.png"
TASKBAND1="$(strip_band "$STATE/strip-task.png" 250 600 100)"
chk "shell: the task chip draws with the window" test "$TASKBAND1" -gt $(( TASKBAND0 + 100 ))
dsp "hl.plugin.awesome.minimize()"; sleep 1
capture_nested "$STATE/strip-task-min.png"
TASKBAND2="$(strip_band "$STATE/strip-task-min.png" 250 600 100)"
chk "shell: minimizing dims the chip" test "$TASKBAND2" -lt "$TASKBAND1"
dsp "hl.plugin.awesome.restore()"; sleep 1
capture_nested "$STATE/strip-task-restore.png"
TASKBAND3="$(strip_band "$STATE/strip-task-restore.png" 250 600 100)"
chk "shell: restoring re-brightens the chip" test "$TASKBAND3" -gt "$TASKBAND2"
FT="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FT" ]] && dsp "hl.dsp.window.close({window=\"address:$FT\"})"; sleep 1
capture_nested "$STATE/strip-task-gone.png"
chk "shell: the chip leaves with the window" \
	test "$(strip_band "$STATE/strip-task-gone.png" 250 600 100)" -le $(( TASKBAND0 + 100 ))

# ---- the bell badge: the counts the bell reads from the model --------------
BELLBAND() { # bellband <img> — the badge fill in the bell's badge box
	# the badge is col_active (0xff9acbff = 154,203,255, a lavender) rounded
	# rect with the dark count on it: the lavender is far from the strip bg
	# (19,39,50) and the bright text (238,243,245, whose r breaks the test).
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
bx = int(sys.argv[2])
n = 0
for y in range(1, 20):
    for x in range(max(0, bx - 6), min(im.size[0], bx + 22)):
        r, g, b = px[x, y]
        if 130 < r < 185 and 180 < g < 230 and b > 235:
            n += 1
print(n)
PY
}
dsp "hl.dsp.exec_cmd('notify-send -a bellp -t 30000 \"badge probe\" body')"; sleep 1.2
capture_nested "$STATE/bell-badge.png"
chk "shell: the badge appears with a bannered card" \
	test "$(BELLBAND "$STATE/bell-badge.png" "$BELL0")" -gt 30
dsp "hl.dsp.exec_cmd('notify-send -a bellp -u critical -t 30000 \"badge probe 2\" body')"; sleep 1.2
capture_nested "$STATE/bell-badge2.png"
# the count itself is the MODEL's business (asserted via state: the badge
# draws LIVE+KEPT, which IS the state live count). The badge holds a 15px
# minimum width below three digits and the 9px glyph moves only ~2 px of
# ink between "1" and "2" — a pixel count cannot see the growth, so the
# pixel side asserts the badge still stands, the state side the number.
chk "shell: the badge count grows with a second card" \
	test "$(st)" = "center:0 live:2 dnd:0" && test "$(BELLBAND "$STATE/bell-badge2.png" "$BELL0")" -gt 30
hq awesome clear >/dev/null; sleep 1.2
capture_nested "$STATE/bell-badge0.png"
chk "shell: the badge leaves at zero" \
	test "$(BELLBAND "$STATE/bell-badge0.png" "$BELL0")" -lt 20

# ---- the tray: the StatusNotifierItem lifecycle -----------------------------
SNIBIN="$REPO/devtools/fake-sni"
sni_items() {
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user get-property org.kde.StatusNotifierWatcher \
		/StatusNotifierWatcher org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>/dev/null
}
icon_box() {
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
panel_extent() {
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
col_h() {
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x = min(int(sys.argv[2]), im.size[0] - 1)
run = [y for y in range(27, im.size[1]) if min(px[x, y][:3]) > 5]
print(max(run) - min(run) + 1 if len(run) > 20 and max(run) - min(run) > 40 else 0)
PY
}

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
ICON="$(icon_box "$STATE/tray-icon.png")"
chk "tray: magenta icon sits in the bar band" bash -c "[[ '${ICON:0:1}' != '0' ]]"
read -r IX IY <<<"$ICON"

# the root menu: doubled + trailing separators collapse to one (parse trim)
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$IX" "$IY" | vp
sleep 1.5
capture_nested "$STATE/tray-root.png"
read -r RW RX0 RX1 RYT RYB <<<"$(panel_extent "$STATE/tray-root.png")"
chk "tray: one panel span open (root only)" test "$RW" -ge 120
chk "tray: one panel span open (root only, no cascade)" test "$RW" -le 260
ROOT_H="$(col_h "$STATE/tray-root.png" "$(( (RX0 + RX1) / 2 ))")"
chk "tray: root panel height 112 (4 rows + 1 collapsed separator)" test "$ROOT_H" -ge 110
chk "tray: root panel height 112 (upper bound)" test "$ROOT_H" -le 116

# the submenu cascade
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
	"$((RX0 + 20))" "$((RYT + 92))" | vp
sleep 1.5
capture_nested "$STATE/tray-sub.png"
read -r SW SX0 SX1 SYT SYB <<<"$(panel_extent "$STATE/tray-sub.png")"
chk "tray: cascade widened the span (sub panel opened)" test "$SW" -ge 290
if [[ "$SX0" -lt "$RX0" ]]; then
	SUBX=$((SX0 + 40))
else
	SUBX=$((RX1 + 40))
fi
SUB_H="$(col_h "$STATE/tray-sub.png" "$SUBX")"
chk "tray: sub panel height 56 (trailing separator trimmed)" test "$SUB_H" -ge 54
chk "tray: sub panel height 56 (upper bound)" test "$SUB_H" -le 60

# outside click closes both
printf 'move 300 400\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' | vp
sleep 1.2
capture_nested "$STATE/tray-closed.png"
chk "tray: outside click closed every panel" \
	test "$(panel_extent "$STATE/tray-closed.png" | awk '{print $1}')" -le 10

# teardown: the item leaves the strip
kill "$FAKE_PID" 2>/dev/null
for _ in $(seq 1 30); do
	kill -0 "$FAKE_PID" 2>/dev/null || break
	sleep 0.2
done
chk "tray: watcher dropped the item" test "$(sni_items | grep -c HyprFakeSNI)" = 0
sleep 1.2
capture_nested "$STATE/tray-gone.png"
chk "tray: icon left the strip" test "$(icon_box "$STATE/tray-gone.png")" = "0 0"

# ---- the menubar: the launcher ---------------------------------------------
# The prompt is a single 26px strip directly under the bar (awesome's
# menubar wibox), so it is measured as a band, not a column span.
open_menubar() { dsp "hl.plugin.awesome.menubar()"; sleep 1; }
close_menubar() { printf 'tap esc\nsleep 400\n' | vk; sleep 0.6; }
menubar_band() { # menubar_band <frame> — bright count in the prompt band (y 27..52)
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
n = 0
for y in range(27, 52):
    for x in range(0, im.size[0], 2):
        if min(px[x, y][:3]) > 60:
            n += 1
print(n)
PY
}
open_menubar
capture_nested "$STATE/menubar-open.png"
MB_OPEN="$(menubar_band "$STATE/menubar-open.png")"
chk "menubar: the prompt opens below the strip" test "$MB_OPEN" -gt 200
# type a filter: the completion list narrows (fewer labels in the band)
printf 'tap c\ntap a\ntap l\ntap c\nsleep 500\n' | vk
sleep 0.8
capture_nested "$STATE/menubar-filter.png"
MB_FILTER="$(menubar_band "$STATE/menubar-filter.png")"
chk "menubar: typing filters the completion list (the band narrows)" test "$MB_OPEN" -gt "$MB_FILTER"
close_menubar
capture_nested "$STATE/menubar-closed.png"
chk "menubar: esc closes the prompt" test "$(menubar_band "$STATE/menubar-closed.png")" -le 50

# the launcher's shared clipboard: the 4 KiB UTF-8 query budget, on the
# monolith's history store. Execute a comment-only tail so the paste is
# side-effect free.
clipboard_now() {
	clipboard_stop
	local log="$STATE/clip-now.log"
	WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 0 "$1" >"$log" 2>&1 & CLIP_PID=$!
	for _ in $(seq 1 30); do grep -qx READY "$log" 2>/dev/null && return; sleep 0.1; done
	echo "instant clipboard source did not reach READY" >&2
	clipboard_stop
	exit 1
}
clipboard_stop() {
	if [[ -n "$CLIP_PID" ]]; then
		kill "$CLIP_PID" 2>/dev/null || true
		wait "$CLIP_PID" 2>/dev/null || true
		CLIP_PID=""
	fi
}
QUERY_PAYLOAD="$(python3 -c 'import sys; sys.stdout.write("true # " + "a" * 4085 + "🙂" + "ignored")')"
clipboard_now "$QUERY_PAYLOAD"
open_menubar
printf 'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none\nsleep 400\n' | vk
sleep 1
chk "launcher: Ctrl+V admits one valid UTF-8 query up to 4 KiB" python3 - "$AW_HISTORY" <<'PY'
import sys
lines = open(sys.argv[1], "rb").read().splitlines()
ok = bool(lines) and len(lines[-1]) == 4096 and lines[-1].decode().startswith("true # ") and lines[-1].endswith("🙂".encode())
raise SystemExit(0 if ok else 1)
PY
clipboard_stop
CLIP_LAUNCH_LOG="$STATE/clip-launch-cancel.log"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 700 stale >"$CLIP_LAUNCH_LOG" 2>&1 & CLIP_PID=$!
for _ in $(seq 1 30); do grep -qx READY "$CLIP_LAUNCH_LOG" 2>/dev/null && break; sleep 0.1; done
open_menubar
printf 'mods ctrl\ntap v\nmods none\ntap esc\nsleep 400\n' | vk
wait "$CLIP_PID" 2>/dev/null || true; CLIP_PID=""; sleep 0.2
clipboard_now true
open_menubar
printf 'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none\nsleep 400\n' | vk
sleep 1
chk "launcher: closing the prompt invalidates a late paste" \
	test "$(tail -n 1 "$AW_HISTORY")" = true
clipboard_stop

chk "shell: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
