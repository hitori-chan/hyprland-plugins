# awesome/gate/shell.sh — the shell module's behavior battery: the strip's
# presence and state (tags, tasks, the bell's dot, the clock), the tray
# lifecycle against the bar, and the menubar/launcher. Helpers:
# capture_nested, vp, vk, chk, hq, ws, dsp from the harness; bar_bright,
# task_band, accent_px, dot_px, tag_cx from lib.sh.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- the strip -------------------------------------------------------------
# The strip warms in over its first frames after launch (the warm/draw gate
# never paints in the frame that creates a texture); a capture that lands
# mid-warm counts the absent glyphs. Wait for the strip to actually draw —
# bounded, so a strip that never draws still fails the check below.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if capture_nested "$STATE/strip-base.png" && strip_drawn "$STATE/strip-base.png"; then
		break
	fi
	sleep 0.5
done
# one reading for the log either way: a failed canary says how much of the
# strip drew (0 = nothing; a theme or face change moves the count)
echo "shell: strip bright=$(bar_bright "$STATE/strip-base.png") bell=$(bell_x "$STATE/strip-base.png")" >&2
chk "shell: the strip is on screen (clock, bell and viewed tag draw)" strip_drawn "$STATE/strip-base.png"
chk "shell: no dot on the bell with nothing to see" test "$(dot_px "$STATE/strip-base.png")" = 0

# ---- tags: the click switches the viewed workspace -------------------------
chk "shell: starts on workspace 1" test "$(ws)" = 1
printf 'move %s 13\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$(tag_cx 3)" | vp
chk "shell: clicking tag 三 switches to workspace 3" until_is 3 3 ws
# off the strip: under the pointer the viewed tag's accent lifts toward white
printf 'move 900 500\nsleep 60\n' | vp
ws3_drawn() { (($(accent_px "$1" 64 90) > 15 && $(accent_px "$1" 8 34) == 0)); }
shot_until 3 "$STATE/strip-ws3.png" ws3_drawn "$STATE/strip-ws3.png"
# the viewed tag is accent text, the rest never accent
chk "shell: the viewed tag draws in the accent" test "$(accent_px "$STATE/strip-ws3.png" 64 90)" -gt 15
chk "shell: a tag not viewed draws no accent" test "$(accent_px "$STATE/strip-ws3.png" 8 34)" = 0
# Fitts: the screen's corner is tag 一's — a click thrown into it views 1
printf 'move 0 0\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' | vp
chk "shell: a click in the top-left corner views workspace 1" until_is 3 1 ws

# ---- tags: hover lifts the text, never a ground ------------------------------
# an empty tag's dim kanji lights to the bar's text under the pointer, and
# the cell's corners stay the strip's own ground
tag_cell() { # tag_cell <frame> <tag> — "<lit glyph px> <corner max channel>"
	python3 - "$1" "$(tag_cx "$2")" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load(); cx = int(sys.argv[2])
lit = sum(1 for y in range(3, 25) for x in range(cx - 13, cx + 13) if max(px[x, y]) > 120)
print(lit, max(max(px[cx - 12, 4]), max(px[cx + 11, 23])))
PY
}
read -r LIT0 CORNER0 <<<"$(tag_cell "$STATE/strip-base.png" 5)"
printf 'move %s 13\nsleep 60\n' "$(tag_cx 5)" | vp
tag5_lit() { local lit; read -r lit _ <<<"$(tag_cell "$1" 5)"; ((lit > LIT0 + 15)); }
shot_until 3 "$STATE/strip-hover5.png" tag5_lit "$STATE/strip-hover5.png"
read -r LIT1 CORNER1 <<<"$(tag_cell "$STATE/strip-hover5.png" 5)"
echo "shell: tag 5 lit $LIT0 -> $LIT1, corner $CORNER0 -> $CORNER1" >&2
chk "shell: hovering an empty tag lifts its text" test "$LIT1" -gt $(( LIT0 + 15 ))
chk "shell: and paints no ground" test $(( CORNER1 - CORNER0 )) -le 3 -a $(( CORNER0 - CORNER1 )) -le 3
# the viewed tag lifts as well: its accent steps toward white
printf 'move %s 13\nsleep 60\n' "$(tag_cx 1)" | vp
tag1_lifted() { (($(accent_px "$1" 8 34) <= 2 && $(lifted_px "$1" 8 34) > 6)); }
chk "shell: hovering the viewed tag lifts its accent" \
	shot_until 3 "$STATE/strip-hover1.png" tag1_lifted "$STATE/strip-hover1.png"
printf 'move 900 500\nsleep 60\n' | vp; settle

# ---- tasks: the title appears with the window, dims with minimize ----------
# focused accent, minimized DIM: task_band counts lit titles (max channel)
TASKBAND0="$(task_band "$STATE/strip-base.png")"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; settled foot
band_over() { (($(task_band "$1") > $2)); }
band_under() { (($(task_band "$1") < $2)); }
shot_until 3 "$STATE/strip-task.png" band_over "$STATE/strip-task.png" $((TASKBAND0 + 100))
TASKBAND1="$(task_band "$STATE/strip-task.png")"
chk "shell: the task chip draws with the window" test "$TASKBAND1" -gt $(( TASKBAND0 + 100 ))
dsp "hl.plugin.awesome.minimize()"
shot_until 3 "$STATE/strip-task-min.png" band_under "$STATE/strip-task-min.png" "$TASKBAND1"
TASKBAND2="$(task_band "$STATE/strip-task-min.png")"
chk "shell: minimizing dims the chip" test "$TASKBAND2" -lt "$TASKBAND1"
dsp "hl.plugin.awesome.restore()"
task_back() { band_over "$1" "$TASKBAND2" && (($(accent_px "$1" 280 900) > 20)); }
shot_until 3 "$STATE/strip-task-restore.png" task_back "$STATE/strip-task-restore.png"
TASKBAND3="$(task_band "$STATE/strip-task-restore.png")"
chk "shell: restoring re-brightens the chip" test "$TASKBAND3" -gt "$TASKBAND2"
# the focused task is accent text, and lifts under the pointer as the rest do
ACC0="$(accent_px "$STATE/strip-task-restore.png" 280 900)"
chk "shell: the focused task draws in the accent" test "$ACC0" -gt 20
printf 'move 400 13\nsleep 60\n' | vp
task_lifted() { (($(accent_px "$1" 280 900) <= 2 && $(lifted_px "$1" 280 900) > 20)); }
chk "shell: hovering the focused task lifts its accent" \
	shot_until 3 "$STATE/strip-task-hover.png" task_lifted "$STATE/strip-task-hover.png"
printf 'move 900 500\nsleep 60\n' | vp; settle
FT="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
close_wait "$FT"
chk "shell: the chip leaves with the window" \
	shot_until 3 "$STATE/strip-task-gone.png" band_under "$STATE/strip-task-gone.png" $((TASKBAND0 + 101))

# ---- the window list: right-click a task -----------------------------------
# every window, any tag (shell/menu.cpp): a row reads as its task does
# (focused accent), and a right-hand column names its workspace's tag. The
# panel is 250 wide, centered on the click, its rows 28 from y 37.
dsp "hl.dsp.focus({workspace=\"2\"})"
dsp "hl.dsp.exec_cmd('foot -a wlother --window-size-pixels=400x200')"; wait_class wlother
dsp "hl.dsp.focus({workspace=\"1\"})"
dsp "hl.dsp.exec_cmd('foot -a wlhere --window-size-pixels=400x200')"; settled wlhere
click 400 13 273
WLX=$((400 - 125))
winlist_drawn() { (($(lit_px "$1" $((WLX + 250 - 30)) $((WLX + 250 - 12)) 37 65) > 10 && $(accent_px "$1" "$WLX" $((WLX + 250)) 65 93) > 20)); }
shot_until 3 "$STATE/winlist.png" winlist_drawn "$STATE/winlist.png"
chk "winlist: the other workspace's window wears its tag" test "$(lit_px "$STATE/winlist.png" $((WLX + 250 - 30)) $((WLX + 250 - 12)) 37 65)" -gt 10
chk "winlist: the focused window's row is accent" test "$(accent_px "$STATE/winlist.png" "$WLX" $((WLX + 250)) 65 93)" -gt 20
chk "winlist: the other window's row is not" test "$(accent_px "$STATE/winlist.png" "$WLX" $((WLX + 250)) 37 65)" = 0
tap esc
for a in $(clients | python3 -c "import json,sys
print(' '.join(c['address'] for c in json.load(sys.stdin) if c['class'] in ('wlother','wlhere')))"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
chk "winlist: its windows are closed" until_is 5 0 pyc "sum(1 for c in cs if c['class'] in ('wlother','wlhere'))"

# ---- the bell's dot: a card the center has not shown -----------------------
dotted() { (($(dot_px "$1") > 8)); }
undotted() { (($(dot_px "$1") == 0)); }
nsend -a bellp -t 30000 "badge probe" body
chk "shell: an unseen card dots the bell" shot_until 3 "$STATE/bell-badge.png" dotted "$STATE/bell-badge.png"
# the center shows it: open, close (the banner re-pops), and the dot is out
hq awesome center >/dev/null
hq awesome center >/dev/null
seen_out() { [[ "$(st)" == "center:0 live:1 dnd:0" ]] && undotted "$STATE/bell-seen.png"; }
chk "shell: the center showed it: the dot is out, the card stays" shot_until 3 "$STATE/bell-seen.png" seen_out
nsend -a bellp -u critical -t 30000 "badge probe 2" body
chk "shell: a new card lights it again" shot_until 3 "$STATE/bell-badge2.png" dotted "$STATE/bell-badge2.png"
hq awesome clear >/dev/null
chk "shell: the dot leaves with the last card" shot_until 3 "$STATE/bell-badge0.png" undotted "$STATE/bell-badge0.png"

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
pts = [(x, y) for x in range(w // 2, w) for y in range(28)
       if px[x, y][0] > 150 and px[x, y][1] < 90 and px[x, y][2] > 150]
if len(pts) < 50:
    print("0 0"); sys.exit()
xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
print((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
PY
}
col_h() {
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x = min(int(sys.argv[2]), im.size[0] - 1)
run = [y for y in range(29, im.size[1]) if min(px[x, y][:3]) > 5]
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
icon_in() { [[ "$(icon_box "$1")" != "0 0" ]]; }
icon_out() { [[ "$(icon_box "$1")" == "0 0" ]]; }
shot_until 4 "$STATE/tray-icon.png" icon_in "$STATE/tray-icon.png"
ICON="$(icon_box "$STATE/tray-icon.png")"
# an empty answer (a failed capture) is no icon either
chk "tray: magenta icon sits in the bar band" bash -c "[[ -n '$ICON' && '${ICON:0:1}' != '0' ]]"
read -r IX IY <<<"$ICON"

# the root menu: doubled + trailing separators collapse to one (parse trim)
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$IX" "$IY" | vp
root_drawn() {
	local w x0 x1 h
	read -r w x0 x1 _ <<<"$(panel_extent "$1")"
	((w >= 120 && w <= 260)) || return 1
	h="$(col_h "$1" "$(((x0 + x1) / 2))")"
	((h >= 129 && h <= 137))
}
shot_until 4 "$STATE/tray-root.png" root_drawn "$STATE/tray-root.png"
read -r RW RX0 RX1 RYT RYB <<<"$(panel_extent "$STATE/tray-root.png")"
chk "tray: one panel span open (root only)" test "$RW" -ge 120
chk "tray: one panel span open (root only, no cascade)" test "$RW" -le 260
ROOT_H="$(col_h "$STATE/tray-root.png" "$(( (RX0 + RX1) / 2 ))")"
# shell.hpp's menu: 2 x PAD 5 + 4 rows x 28 + 1 separator 9 = 131, + the rim;
# a kept doubled or trailing separator is another 9
chk "tray: root panel height 133 (4 rows + 1 collapsed separator)" test "$ROOT_H" -ge 129
chk "tray: root panel height 133 (upper bound)" test "$ROOT_H" -le 137

# the submenu cascade
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
	"$((RX0 + 20))" "$((RYT + 112))" | vp
sub_x() { # the sub panel's sample column, beside the root on either side
	local w x0
	read -r w x0 _ <<<"$(panel_extent "$1")"
	((x0 < RX0)) && echo $((x0 + 40)) || echo $((RX1 + 40))
}
sub_drawn() {
	local w h
	read -r w _ <<<"$(panel_extent "$1")"
	((w >= 290)) || return 1
	h="$(col_h "$1" "$(sub_x "$1")")"
	((h >= 64 && h <= 71))
}
shot_until 4 "$STATE/tray-sub.png" sub_drawn "$STATE/tray-sub.png"
read -r SW SX0 SX1 SYT SYB <<<"$(panel_extent "$STATE/tray-sub.png")"
chk "tray: cascade widened the span (sub panel opened)" test "$SW" -ge 290
if [[ "$SX0" -lt "$RX0" ]]; then
	SUBX=$((SX0 + 40))
else
	SUBX=$((RX1 + 40))
fi
SUB_H="$(col_h "$STATE/tray-sub.png" "$SUBX")"
chk "tray: sub panel height 68 (trailing separator trimmed)" test "$SUB_H" -ge 64
chk "tray: sub panel height 68 (upper bound)" test "$SUB_H" -le 71

# outside click closes both
printf 'move 300 400\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' | vp
panels_shut() { (($(panel_extent "$1" | awk '{print $1}') <= 10)); }
panel_open() { (($(panel_extent "$1" | awk '{print $1}') >= 120)); }
chk "tray: outside click closed every panel" \
	shot_until 3 "$STATE/tray-closed.png" panels_shut "$STATE/tray-closed.png"

# Esc closes an open menu
printf 'move %s %s\nsleep 40\npress 272\nsleep 40\nrelease 272\nsleep 80\n' "$IX" "$IY" | vp
chk "tray: the menu reopened for esc" shot_until 4 "$STATE/tray-reopen.png" panel_open "$STATE/tray-reopen.png"
tap esc
chk "tray: esc closed the menu" shot_until 3 "$STATE/tray-esc.png" panels_shut "$STATE/tray-esc.png"

# attention with no attention icon of its own (most apps): the plain one
# stays, not a blank cell
kill -USR1 "$FAKE_PID" 2>/dev/null
sleep 1.2
capture_nested "$STATE/tray-attn.png"
ICON="$(icon_box "$STATE/tray-attn.png")"
chk "tray: an attention item without an attention icon keeps its own" bash -c "[[ -n '$ICON' && '${ICON:0:1}' != '0' ]]"

# ---- the strip's cells: every widget owns its own ---------------------------
# A widget with nothing to click still owns its cell (shell/strip.cpp): the
# battery's clicks once went to the tray item beside it, and the clock's
# opened the center. With a task on the strip, the cells tile it from edge
# to edge — none overlapping (the tray's outer icons once reached into the
# last task), no dead pixel between them (once 7 px between the tags and
# the tasks). `cells` is the gate build's map.
dsp "hl.dsp.exec_cmd('foot -a cellsprobe --window-size-pixels=400x200')"; settled cellsprobe
CELLS="$(hq awesome cells)"
echo "cells: $(tr '\n' ';' <<<"$CELLS")" >&2
cells_tile() {
	python3 -c '
import sys
w = int(sys.argv[1])
cells = [l.split() for l in sys.stdin.read().splitlines()]
cells = [(c[0], int(c[1]), int(c[2])) for c in cells if len(c) == 3]
names = [n for n, _, _ in cells]
ok = cells and cells[0][1] == 0 and cells[-1][2] == w and "task" in names and names[-1] == "clock"
ok = ok and all(a < b for _, a, b in cells) and all(cells[i][1] == cells[i - 1][2] for i in range(1, len(cells)))
print(1 if ok else 0)
' "$MON_W" <<<"$CELLS"
}
cell_mid() { awk -v w="$1" '$1 == w { print int(($2 + $3) / 2); exit }' <<<"$CELLS"; }
chk "cells: they tile the strip edge to edge, none overlapping" test "$(cells_tile)" = 1
# the right slot's order, macOS's: the bell, the switches, the battery (where
# the host has one), the clock at the edge
right_order() { awk '$1 ~ /^(bell|controls|battery|clock)$/ { printf "%s ", $1 }' <<<"$CELLS"; }
RIGHT_WANT="bell controls clock "
compgen -G "/sys/class/power_supply/BAT*" >/dev/null && RIGHT_WANT="bell controls battery clock "
chk "cells: the right slot runs bell, switches, battery, clock" test "$(right_order)" = "$RIGHT_WANT"
# the clicks over the bare desktop: a tray menu is measured below the strip,
# where the probe's window would read as one (the right slot's cells stay)
CP="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='cellsprobe'), ''))")"
close_wait "$CP"
if compgen -G "/sys/class/power_supply/BAT*" >/dev/null; then
	BATX="$(cell_mid battery)"
	chk "cells: the battery owns a cell" test -n "$BATX"
	click "${BATX:-0}" 13 272
	sleep 1
	capture_nested "$STATE/cells-battery.png"
	chk "cells: a click on the battery opens no tray menu" test "$(panel_extent "$STATE/cells-battery.png" | awk '{print $1}')" -le 10
	tap esc
else
	echo "cells: this host has no system battery (its click is not checked)" >&2
fi
click "$(cell_mid clock)" 13 272
center_shut() { [[ "$(st)" == "center:0 "* ]]; }
chk "cells: a click on the clock leaves the center shut" center_shut
center_shut || hq awesome center >/dev/null

# ---- the control center's door: the bar's switches -------------------------
# Right of the bell (shell/bell.cpp): a click opens the control center on its
# bar's monitor and lights the glyph (the accent, as text); one panel holds
# the column at a time, the bell taking it back; a click off every surface
# closes it.
CCX="$(cell_mid controls)"
chk "controls: the switches own a cell" test "${CCX:-0}" -gt 0
cols() { echo "$(st | cut -d' ' -f1) $(hq awesome controlcenter-shown)"; } # "center:<0|1> <control center 0|1>"
click "${CCX:-0}" 13 272
chk "controls: a click on the switches opens the control center" until_is 3 "center:0 1" cols
cc_lit() { (($(accent_px "$1" $((CCX - 8)) $((CCX + 8))) > 10)); }
chk "controls: ...and lights them" shot_until 3 "$STATE/cc-lit.png" cc_lit "$STATE/cc-lit.png"
click "$(cell_mid bell)" 13 272
chk "controls: the bell takes the column: the center opens, the control center shuts" until_is 3 "center:1 0" cols
click "${CCX:-0}" 13 272
chk "controls: ...and the switches take it back" until_is 3 "center:0 1" cols
outside_click
chk "controls: a click off every surface closes it" until_is 3 "center:0 0" cols

# ---- the strip's glass: live over a terminal taking typing -----------------
# The glass live-blurs what lies under it: the prompt's row right below
# reaches its blur, and each keystroke moves the strip on. A partial
# repaint of it shimmered: the repainted part's blur sampled the rest of
# the strip as each swapchain buffer kept it (its own old glass), and the
# presented frames alternated between two strips (core/canvas.hpp,
# wholeGlass). Only the private host's output shows the presented
# buffers; at rest it must show the same strip as the nested's own capture
# (else the host's view moved and the check would measure nothing).
dsp "hl.dsp.exec_cmd('foot -a glassprobe --window-size-pixels=400x200')"; settled glassprobe
dsp "hl.plugin.awesome.maximize()"; settled glassprobe
printf 'move %d %d\nsleep 60\n' $((MON_W / 2)) $((MON_H / 2)) | vp
glass_rest() {
	capture_nested "$STATE/glass-nested.png" && capture_host "$STATE/glass-rest.png" &&
		(($(strip_diff "$STATE/glass-nested.png" "$STATE/glass-rest.png") <= 3))
}
chk "glass: the host shows the nested's strip" until_ok 3 glass_rest
# the typing outlasts the captures (24 at ~70 ms)
printf 'tap a\nsleep 45\n%.0s' $(seq 1 40) | vk &
TYPER=$!
sleep 0.3
for i in $(seq -w 1 24); do capture_host "$STATE/glass-$i.png"; done
wait "$TYPER" 2>/dev/null
read -r GS GF GR <<<"$(strip_states "$STATE"/glass-[0-9]*.png)"
echo "glass: $GS strip states over $GF distinct frames, $GR returns" >&2
chk "glass: the typing presented frames" test "${GF:-0}" -ge 8
chk "glass: the strip frosts the terminal under it (it follows the typing)" test "${GS:-0}" -ge 2
chk "glass: the strip never shimmers back to a state it left" test "${GR:-1}" = 0
GP="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='glassprobe'), ''))")"
close_wait "$GP"

# teardown: the item leaves the strip
kill "$FAKE_PID" 2>/dev/null
for _ in $(seq 1 30); do
	kill -0 "$FAKE_PID" 2>/dev/null || break
	sleep 0.2
done
chk "tray: watcher dropped the item" test "$(sni_items | grep -c HyprFakeSNI)" = 0
chk "tray: icon left the strip" shot_until 3 "$STATE/tray-gone.png" icon_out "$STATE/tray-gone.png"

# ---- the menubar: the launcher ---------------------------------------------
# The prompt is a single bar-high strip directly under the bar (awesome's
# menubar wibox), so it is measured as a band, not a column span.
open_menubar() { dsp "hl.plugin.awesome.menubar()"; settle; } # it opens through a hop
close_menubar() { tap esc; }
menubar_band() { # menubar_band <frame> — bright count in the prompt band (y 29..56)
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
n = 0
for y in range(29, 56):
    for x in range(0, im.size[0], 2):
        if min(px[x, y][:3]) > 60:
            n += 1
print(n)
PY
}
open_menubar
band_above() { (($(menubar_band "$1") > $2)); }
band_below() { (($(menubar_band "$1") < $2)); }
# the band holds the completions once the desktop index's scan landed: let
# two captures agree on it, or a list still filling reads as narrowed
mb_full() { band_above "$1" 200 && [[ "$(menubar_band "$1")" == "${MB_LAST:-}" ]] || { MB_LAST="$(menubar_band "$1")"; false; }; }
shot_until 4 "$STATE/menubar-open.png" mb_full "$STATE/menubar-open.png"
MB_OPEN="$(menubar_band "$STATE/menubar-open.png")"
chk "menubar: the prompt opens below the strip" test "$MB_OPEN" -gt 200
# type a filter: the completion list narrows (fewer labels in the band)
printf 'tap c\ntap a\ntap l\ntap c\n' | vk
chk "menubar: typing filters the completion list (the band narrows)" \
	shot_until 3 "$STATE/menubar-filter.png" band_below "$STATE/menubar-filter.png" "$MB_OPEN"
close_menubar
chk "menubar: esc closes the prompt" shot_until 3 "$STATE/menubar-closed.png" band_below "$STATE/menubar-closed.png" 51

# Tab on a command word typed as a path completes from its own directory,
# executables only ($PATH holds no slashed names): the run marker proves
# the completed line ran, and the plain file sorting first was skipped
vtype() { # lowercase letters, digits, '/', '-' and '.' as vkbd taps
	local s=$1 i c
	for ((i = 0; i < ${#s}; i++)); do
		c=${s:i:1}
		case $c in
		/) echo "tap 53" ;;
		-) echo "tap 12" ;;
		.) echo "tap 52" ;;
		0) echo "tap 11" ;;
		[1-9]) echo "tap $((c + 1))" ;;
		[a-z]) echo "tap $c" ;;
		*) return 1 ;;
		esac
	done
}
mkdir -p "$STATE/tabrun"
printf '#!/bin/sh\n: > "%s/tabrun/ran"\n' "$STATE" >"$STATE/tabrun/gatetabrun"
chmod +x "$STATE/tabrun/gatetabrun"
: >"$STATE/tabrun/gatetabdoc"
rm -f "$STATE/tabrun/ran"
open_menubar
{ vtype "$STATE/tabrun/gatetab"; printf 'sleep 100\ntap tab\nsleep 500\ntap enter\n'; } | vk
chk "menubar: tab completes a command typed as a path" until_ok 3 test -e "$STATE/tabrun/ran"

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
printf 'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none\n' | vk
hist_4k() {
	python3 - "$AW_STATE" <<'PY'
import sys
lines = open(sys.argv[1], "rb").read().splitlines()
ok = False
if lines and lines[-1].startswith(b"history\t"):
    payload = lines[-1][len(b"history\t"):]
    ok = len(payload) == 4096 and payload.decode().startswith("true # ") and payload.endswith("🙂".encode())
raise SystemExit(0 if ok else 1)
PY
}
chk "launcher: Ctrl+V admits one valid UTF-8 query up to 4 KiB" until_ok 3 hist_4k
clipboard_stop
CLIP_LAUNCH_LOG="$STATE/clip-launch-cancel.log"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 700 stale >"$CLIP_LAUNCH_LOG" 2>&1 & CLIP_PID=$!
for _ in $(seq 1 30); do grep -qx READY "$CLIP_LAUNCH_LOG" 2>/dev/null && break; sleep 0.1; done
open_menubar
printf 'mods ctrl\ntap v\nmods none\ntap esc\nsleep 400\n' | vk
wait "$CLIP_PID" 2>/dev/null || true; CLIP_PID=""; sleep 0.2
clipboard_now true
open_menubar
printf 'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none\n' | vk
chk "launcher: closing the prompt invalidates a late paste" \
	until_is 3 true bash -c "tail -n 1 '$AW_STATE' | cut -f2-"
clipboard_stop

chk "shell: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
