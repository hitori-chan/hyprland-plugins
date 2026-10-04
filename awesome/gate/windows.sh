# awesome/gate/windows.sh — the windows module's behavior battery: spawn
# placement (the fresh store the migration fed), the CSD geometry battery,
# and maximize/minimize/restore round-trips. The focus-policy battery lives
# in focus.sh and the hostile state-file battery in state.sh (2026-10-04
# gate trim: both are battle-tested and pay for relaunches, so a geometry
# change runs this file alone).

# ---- placement memory ---------------------------------------------------
# The preflight's seeded legacy store migrated to $AW_SPOT at this first
# init; the remembered 500x400 at (100,100) must win over the requested size.
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "size memory: remembered 500x400 beats requested 600x300 at (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "sibling is born at the remembered 500x400 too" \
	"sum(1 for c in cs if c['class']=='foot' and c['size']==[500,400])==2"
expect "sibling lands off the taken spot — no exact stacking" \
	"len(set(tuple(c['at']) for c in cs if c['class']=='foot'))==2"
B="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at'] != [100,100]), ''))")"
A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at']==[100,100]), ''))")"
dsp "hl.dsp.window.close({window=\"address:$A\"})"
for _ in $(seq 1 30); do
	LEFT="$(clients | python3 -c "
import json,sys
address = sys.argv[1]
print(any(c['address'] == address for c in json.load(sys.stdin)))" "$A")"
	[[ "$LEFT" != 1 ]] && break
	sleep 0.1
done
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
placementReclaim() {
	clients | python3 -c "
import json,sys
cs = json.load(sys.stdin)
b = next((c for c in cs if c['address'] == '$B'), None)
def overlaps(c):
    return c['at'][0] < 600 and c['at'][0] + c['size'][0] > 100 and c['at'][1] < 500 and c['at'][1] + c['size'][1] > 100
on_spot = any(c['class'] == 'foot' and c['at'] == [100,100] and c['size'] == [500,400] for c in cs)
positions = {tuple(c['at']) for c in cs if c['class'] == 'foot'}
print(1 if b and len(positions) == 2 and ((not overlaps(b) and on_spot) or (overlaps(b) and not on_spot)) else 0)"
}
chk "freed spot reuse respects remaining sibling geometry" test "$(placementReclaim)" = 1

# fullscreen roundtrip on the focused (newest) foot
feet() { clients | python3 -c "
import json,sys
print(sorted((c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot'))"; }
FEET="$(feet)"
dsp "hl.dsp.window.fullscreen()"; sleep 0.7; dsp "hl.dsp.window.fullscreen()"; sleep 0.9
chk "fullscreen roundtrip restores the exact boxes" test "$(feet)" = "$FEET"

# ---- close storm + memory update ---------------------------------------
storm_jobs=()
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true; sleep 1.2
chk "close storm: no stragglers" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
# kind-exact: a maximize in an earlier battery (quick) leaves a windowed
# restore row for the same class — the coalesced-save invariant is about
# the SPOT row
chk "tsv: exactly one foot spot row survives the coalesced save" \
	test "$(grep -c $'^spot\t[0-9]*\t[0-9]*\t[0-9]*\t[0-9]*\tfoot$' "$AW_STATE")" = 1
chk "tsv: no temp-file debris" bash -c "! ls $AWSTATE/*.tmp 2>/dev/null | grep -q ."

# ---- spawn storm --------------------------------------------------------
storm_jobs=()
for i in $(seq 1 8); do
	dsp "hl.dsp.exec_cmd('foot --window-size-pixels=$((400 + (i % 4) * 80))x$((250 + (i % 3) * 60))')" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true; sleep 2.5
expect "spawn storm: all 8 up, fully inside the workarea" \
	"sum(1 for c in cs if c['class']=='foot')==8 and all(c['at'][0]>=0 and c['at'][1]>=26 and c['at'][0]+c['size'][0]<=$MON_W and c['at'][1]+c['size'][1]<=$MON_H for c in cs if c['class']=='foot')"
storm_jobs=()
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true; sleep 1.2

# ---- fixed-size (dialog/splash) placement --------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=700x500')"; sleep 2
dsp "hl.dsp.exec_cmd('$REPO/devtools/fixwin 310 360')"; sleep 1.5
expect "fixed-size dialog keeps the centered native spot, not the least-overlap corner" \
	"any(abs(c['at'][0]-$(( (MON_W-310)/2 )))<=14 and abs(c['at'][1]-$(( 26+(MON_H-26-360)/2 )))<=14 for c in cs if c['class']=='fixwin')"
FW="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='fixwin'), ''))")"
[[ -n "$FW" ]] && dsp "hl.dsp.window.close({window=\"address:$FW\"})"; sleep 1
chk "fixed-size dialog never writes the class row" \
	bash -c "! grep -q $'\tfixwin\$' \"$AW_STATE\""
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "fixed-size battery left no windows" \
	test "$(pyc "sum(1 for c in cs if c['class'] in ('fixwin','foot'))")" = 0

# ---- CSD geometry offset (discord-updater splash shape) ------------------
# The window box is the CONTENT frame: the client declares its content
# rectangle inside a bigger buffer (the CSD shadow margin), and the
# compositor shows exactly that rectangle at the box — the margin is
# cropped, never padding inside the box nor a halo outside it. No gap
# between the border and the content, on any side; the size round-trips in
# the content frame.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 csdpin')"; sleep 2
expect "pinned CSD splash: box is the content frame (300x350), centered" \
	"any(c['class']=='csdpin' and c['floating'] and c['size']==[300,350] and abs(c['at'][0]-$(( (MON_W-300)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-350)/2 )))<=14 for c in cs)"
box4() { clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class']=='$1'), None)
print(f\"{c['at'][0]} {c['at'][1]} {c['size'][0]} {c['size'][1]}\") if c else print('none')" ; }
capture_nested "$STATE/csd-pin.png"
chk "pinned CSD: content fills the box, the margin is cropped" \
	test "$(python3 - "$STATE/csd-pin.png" "$(box4 csdpin)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
x, y, w, h = map(int, sys.argv[2].split())
MAROON = (48, 32, 32)
# 3px in from the box edge: content, at every corner and the center (the
# 1px border sits on the box edge itself)
content = lambda dx, dy: px[x + dx, y + dy] == MAROON
ok = content(3, 3) and content(w - 4, 3) and content(3, h - 4) and content(w - 4, h - 4) \
   and content(w // 2, h // 2)
# 5px OUTSIDE the box edge: never content
outside = lambda dx, dy: px[dx, dy] != MAROON
ok = ok and outside(x - 5, y + h // 2) and outside(x + w + 5, y + h // 2) \
      and outside(x + w // 2, y - 5) and outside(x + w // 2, y + h + 5)
print(1 if ok else 0)
PY
)" = 1
CP="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdpin'), ''))")"
[[ -n "$CP" ]] && dsp "hl.dsp.window.close({window=\"address:$CP\"})"; sleep 1
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 csdresz - - resz')"; sleep 2
expect "resizable CSD first window still tiles the workarea (pin-skip does not leak)" \
	"any(c['class']=='csdresz' and not c['floating'] and c['at']==[1,31] and c['size']==[$((MON_W-2)), $((MON_H-32))] for c in cs)"
CR="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdresz'), ''))")"
[[ -n "$CR" ]] && dsp "hl.dsp.window.close({window=\"address:$CR\"})"; sleep 1
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 csdpinx - - - - pinx')"; sleep 2
expect "per-axis-pinned CSD: box is the content frame on both axes" \
	"any(c['class']=='csdpinx' and c['floating'] and c['size']==[300,350] for c in cs)"
CX="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdpinx'), ''))")"
[[ -n "$CX" ]] && dsp "hl.dsp.window.close({window=\"address:$CX\"})"; sleep 1

# a FOLLOWING CSD client (real GTK shape): it resizes its content to the
# configure, so any frame mismatch shows up as a clipped or shrunk content.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfollow - - resz vismargin follow')"; sleep 2
expect "following CSD: box is the content frame (800x500), centered" \
	"any(c['class']=='csdfollow' and c['floating'] and c['size']==[800,500] and abs(c['at'][0]-$(( (MON_W-800)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-500)/2 )))<=14 for c in cs)"
capture_nested "$STATE/csd-follow.png"
chk "following CSD: content whole to the box edge, margin cropped" \
	test "$(python3 - "$STATE/csd-follow.png" "$(box4 csdfollow)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
x, y, w, h = map(int, sys.argv[2].split())
# the client resized its content to the configure: content reaches every
# box edge (not clipped, not shrunken), and the vismargin gray the client
# paints in its buffer margin is never drawn — not inside the box, not
# outside it.
content = lambda dx, dy: px[x + dx, y + dy] == (48, 32, 32)
margin  = lambda dx, dy: px[dx, dy] == (176, 176, 176)
ok = content(3, 3) and content(w - 4, 3) and content(3, h - 4) and content(w - 4, h - 4) \
   and content(w // 2, h // 2) \
   and not margin(x - 5, y + h // 2) and not margin(x + w + 5, y + h // 2) \
   and not margin(x + w // 2, y - 5) and not margin(x + w // 2, y + h + 5)
print(1 if ok else 0)
PY
)" = 1
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfollow'), ''))")"
dsp "hl.dsp.window.close({window=\"address:$CF\"})"; sleep 1
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfollow - - resz vismargin follow')"; sleep 2
expect "following CSD: the remembered spot round-trips at the content-frame size" \
	"any(c['class']=='csdfollow' and c['size']==[800,500] and c['at']==[$(( (MON_W-800)/2 )), $(( 30+(MON_H-30-500)/2 ))] for c in cs)"
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfollow'), ''))")"
[[ -n "$CF" ]] && dsp "hl.dsp.window.close({window=\"address:$CF\"})"; sleep 1

# a close in maximized state must mint the spot from the app's last
# windowed box (a browser that always closes maximized keeps its memory);
# the fresh class has only ever been maximized, so a spot row proves the
# fallback ran.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfall - - resz vismargin follow')"; sleep 2
dsp "hl.plugin.awesome.maximize()"; sleep 1
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfall'), ''))")"
dsp "hl.dsp.window.close({window=\"address:$CF\"})"; sleep 1.5
chk "maximized close mints the spot from the last windowed box" \
	grep -q "^spot	[0-9]*	[0-9]*	[0-9]*	[0-9]*	csdfall$" "$AW_STATE"
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfall - - resz vismargin follow')"; sleep 2
expect "minted spot lands the respawn at the remembered box" \
	"any(c['class']=='csdfall' and c['size']==[800,500] and c['at']==[$(( (MON_W-800)/2 )), $(( 30+(MON_H-30-500)/2 ))] for c in cs)"
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfall'), ''))")"
[[ -n "$CF" ]] && dsp "hl.dsp.window.close({window=\"address:$CF\"})"; sleep 1
chk "csd battery left no windows" test "$(pyc "sum(1 for c in cs if c['class'] in ('csdpin','csdresz','csdpinx','csdfollow','csdfall'))")" = 0

# ---- state churn --------------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
box() { clients | python3 -c "
import json,sys
f=[ (c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot' ]
print(f[0] if f else 'none')"; }
REF="$(box)"
chk "churn probe up" test "$REF" != none
for _ in $(seq 1 10); do
	hq seterror disable >/dev/null 2>&1
	sleep 0.1
done
dsp "hl.plugin.awesome.maximize()"; sleep 0.5
MAX_BEFORE="$(box)"
RESERVED_BEFORE="$(reserved)"
ERROR_MESSAGE="reserved-area-probe $(printf 'wrapped-message-with-enough-width-to-force-a-second-line %.0s' {1..6})"
hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
for frame_try in $(seq 1 200); do
	RESERVED_WITH_ERROR="$(reserved)"
	[[ "$RESERVED_WITH_ERROR" != "$RESERVED_BEFORE" ]] && break
	hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
	(( frame_try % 10 == 0 )) && capture_nested "$STATE/error-overlay-frame.png" || true
	sleep 0.1
done
if [[ "$RESERVED_WITH_ERROR" != "$RESERVED_BEFORE" ]]; then
	ok "fork: error overlay changes native reserved area"
else
	bad "fork: error overlay changes native reserved area"
fi
for _ in $(seq 1 30); do
	MAX_WITH_ERROR="$(box)"
	[[ "$MAX_WITH_ERROR" != "$MAX_BEFORE" ]] && break
	sleep 0.1
done
if [[ "$MAX_WITH_ERROR" != "$MAX_BEFORE" ]]; then
	ok "maximize: native reserved-area change reflows maximized geometry"
else
	bad "maximize: native reserved-area change reflows maximized geometry"
fi
hq seterror disable >/dev/null 2>&1
RESERVED_AFTER_ERROR=""
for frame_try in $(seq 1 200); do
	RESERVED_AFTER_ERROR="$(reserved)"
	[[ "$RESERVED_AFTER_ERROR" == "$RESERVED_BEFORE" ]] && break
	hq seterror disable >/dev/null 2>&1
	(( frame_try % 10 == 0 )) && capture_nested "$STATE/error-overlay-frame.png" || true
	sleep 0.1
done
if [[ "$RESERVED_AFTER_ERROR" == "$RESERVED_BEFORE" ]]; then
	ok "fork: disabling error overlay restores native reserved area"
else
	bad "fork: disabling error overlay restores native reserved area"
	printf '      expected reserved=%s, got=%s\n' "$RESERVED_BEFORE" "$RESERVED_AFTER_ERROR"
fi
for _ in $(seq 1 30); do
	[[ "$(box)" == "$MAX_BEFORE" ]] && break
	sleep 0.1
done
chk "maximize: removing native reservation restores maximized workarea" test "$(box)" = "$MAX_BEFORE"
dsp "hl.plugin.awesome.maximize()"; sleep 0.5
chk "maximize: reserved-area roundtrip preserves windowed restore" test "$(box)" = "$REF"
for i in $(seq 1 10); do dsp "hl.plugin.awesome.maximize()"; done; sleep 1
chk "10 maximize toggles round-trip losslessly" test "$(box)" = "$REF"
for i in $(seq 1 5); do dsp "hl.plugin.awesome.minimize()"; dsp "hl.plugin.awesome.restore()"; done; sleep 1
chk "5 minimize/restore cycles round-trip" test "$(box)" = "$REF"
for i in $(seq 1 15); do dsp "hl.dsp.focus({workspace=\"$(( (i % 9) + 1 ))\"})"; done
dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 1
chk "15 workspace hops: back on 1" test "$(ws)" = 1

# the churn probe's foot stays open: close it so the battery leaves the
# desktop clean (the hostile-state relaunch used to take it with the
# instance — state.sh still does, when it runs after this one)
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "windows: its foot is closed at the battery's end" \
	test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0

