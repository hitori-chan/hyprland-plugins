# ---- placement memory ---------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "size memory: remembered 500x400 beats requested 600x300 at (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "sibling is born at the remembered 500x400 too" \
	"sum(1 for c in cs if c['class']=='foot' and c['size']==[500,400])==2"
expect "sibling lands off the taken spot — no exact stacking" \
	"len(set(tuple(c['at']) for c in cs if c['class']=='foot'))==2"
# The spot is occupied, not retired: free it and check the next sibling's
# result. On a small nested output the remaining 500x400 sibling may still
# overlap the remembered box; in that case the least-overlap fallback is the
# correct result, and the test must not demand an impossible free placement.
B="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at'] != [100,100]), ''))")"
A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at']==[100,100]), ''))")"
dsp "hl.dsp.window.close({window=\"address:$A\"})"
# The close dispatch is asynchronous. Do not let the next spawn race the
# unmap and mistake the still-mapped owner for a permanently occupied spot.
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
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 1.2
chk "close storm: no stragglers" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
chk "tsv: exactly one foot row survives the coalesced save" test "$(grep -c $'\tfoot$' "$STATE/hyprplace/lastspot.tsv")" = 1
chk "tsv: no temp-file debris" bash -c "! ls $STATE/hyprplace/*.tmp 2>/dev/null | grep -q ."

# ---- spawn storm --------------------------------------------------------
# 8 concurrent spawns still stress the tiler, the placement fallback and the
# workarea clamp; the count buys nothing above the tile budget.
for i in $(seq 1 8); do
	dsp "hl.dsp.exec_cmd('foot --window-size-pixels=$((400 + (i % 4) * 80))x$((250 + (i % 3) * 60))')" &
done; wait; sleep 2.5
expect "spawn storm: all 8 up, fully inside the workarea" \
	"sum(1 for c in cs if c['class']=='foot')==8 and all(c['at'][0]>=0 and c['at'][1]>=26 and c['at'][0]+c['size'][0]<=$MON_W and c['at'][1]+c['size'][1]<=$MON_H for c in cs if c['class']=='foot')"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 1.2

# ---- fixed-size (dialog/splash) placement --------------------------------
# A fixed-size native toplevel (min == max) keeps the compositor's centered
# native spot and never reads or writes the class row — the Discord-updater
# shape: its grant-exempt main window lets the splash own the row, the
# blocked spot steers it to the least-overlap corner, and the corner is
# re-remembered on close. With a blocker on screen, the old code's
# least-overlap fallback would corner it; the row must also stay unwritten.
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=700x500')"; sleep 2
dsp "hl.dsp.exec_cmd('$REPO/devtools/fixwin 310 360')"; sleep 1.5
expect "fixed-size dialog keeps the centered native spot, not the least-overlap corner" \
	"any(abs(c['at'][0]-$(( (MON_W-310)/2 )))<=14 and abs(c['at'][1]-$(( 26+(MON_H-26-360)/2 )))<=14 for c in cs if c['class']=='fixwin')"
FW="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='fixwin'), ''))")"
[[ -n "$FW" ]] && dsp "hl.dsp.window.close({window=\"address:$FW\"})"; sleep 1
chk "fixed-size dialog never writes the class row" \
	bash -c "! grep -q $'\tfixwin\$' \"$STATE/hyprplace/lastspot.tsv\""
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "fixed-size battery left no windows" \
	test "$(pyc "sum(1 for c in cs if c['class'] in ('fixwin','foot'))")" = 0

# ---- notification cap ---------------------------------------------------
# 55 is just over the cap of 50 — enough to prove eviction without paying
# for 65 daemons.
for i in $(seq 1 55); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" &
done; wait; sleep 4
chk "notif storm: cap holds at exactly 50/55" test "$(hq hyprnotify count)" = 50
# The shade has no history: the evicted entries and the removed control
# verbs must stay gone.
chk "no history verb survives the model removal" test "$(hq hyprnotify history)" = "unknown request"
chk "no recall verb survives the model removal" test "$(hq hyprnotify recall)" = "unknown request"
# wrong-typed hints make sdbus-c++ throw inside the plugin's parse — the
# catch must survive (exercises exception unwinding across the .so boundary).
# Cards expire on their own clocks, so assert the daemon still answers with
# a number, not any absolute count.
dsp "hl.dsp.exec_cmd('notify-send -h int:transient:1 -h string:urgency:critical typed-hint-abuse body')"; sleep 1.5
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" hq_matches '^[0-9]+$' hyprnotify count

# ---- state churn --------------------------------------------------------
# the spawn box is whatever memory dictates after the storm above — capture
# it, then assert every churn round-trips back to exactly that box
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
box() { clients | python3 -c "
import json,sys
f=[ (c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot' ]
print(f[0] if f else 'none')"; }
REF="$(box)"
chk "churn probe up" test "$REF" != none
# Plugin/config startup can leave an error overlay already reserving this
# edge. Clear it inside the nested instance before taking the baseline; a
# replacement of similar wrapped height is not evidence that reservation
# creation failed.
for _ in $(seq 1 10); do
	# The fork's disabled-animation destroy path needs a second render to
	# release the reservation. Reissuing disable is idempotent and requests
	# that frame even when the nested output is otherwise idle.
	hq seterror disable >/dev/null 2>&1
	sleep 0.1
done
# A plugin-maximized window lives outside the compositor fullscreen model.
# Changing a native reserved area must therefore reach hyprmax explicitly.
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
MAX_BEFORE="$(box)"
RESERVED_BEFORE="$(reserved)"
ERROR_MESSAGE="reserved-area-probe $(printf 'wrapped-message-with-enough-width-to-force-a-second-line %.0s' {1..6})"
hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
# The nested Wayland output lives on an off-screen headless host monitor. When
# both compositors are otherwise idle, the outer frame callback can arrive
# beyond ten seconds even though queueCreate() requested a full render. Keep
# this value-based and bounded, but allow twenty seconds so
# the reservation probe measures state instead of host refresh timing. A
# periodic screencopy request drives the off-screen output through the queued
# render path; destruction needs two such frames when animations are disabled.
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
	ok "hyprmax: native reserved-area change reflows maximized geometry"
else
	bad "hyprmax: native reserved-area change reflows maximized geometry"
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
chk "hyprmax: removing native reservation restores maximized workarea" test "$(box)" = "$MAX_BEFORE"
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
chk "hyprmax: reserved-area roundtrip preserves windowed restore" test "$(box)" = "$REF"
for i in $(seq 1 10); do dsp "hl.plugin.hyprmax.toggle()"; done; sleep 1
chk "10 maximize toggles round-trip losslessly" test "$(box)" = "$REF"
for i in $(seq 1 5); do dsp "hl.plugin.hyprbar.minimize()"; dsp "hl.plugin.hyprbar.restore()"; done; sleep 1
chk "5 minimize/restore cycles round-trip" test "$(box)" = "$REF"
for i in $(seq 1 15); do dsp "hl.dsp.focus({workspace=\"$(( (i % 9) + 1 ))\"})"; done
dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 1
chk "15 workspace hops: back on 1" test "$(ws)" = 1

# ---- hostile state file -------------------------------------------------
kill_nested
printf 'garbage\n42\n1e400\t0\t300\t200\tinffoot\n-100\t-100\t-50\t-50\tnegfoot\n100000\t100000\t400\t300\tfoot\n' > "$STATE/hyprplace/lastspot.tsv"
# the policy store is the other user-editable file: a verb-less line, an
# empty key, an unknown verb and a duplicated rule must all be skipped or
# deduped, not fatal (a long key is well-formed: app names carry no cap)
mkdir -p "$STATE/hyprnotify"
printf 'garbage\ns\n s\tx\nz\tnope\ns\t\np\t\ns\tkeepme\ns\tkeepme\n' > "$STATE/hyprnotify/policy.tsv"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "hostile tsv: all 8 plugins still load" test "$(hq plugin list | grep -c Plugin)" = 8
chk "hostile policy: only the well-formed rule loaded" test "$(hq hyprnotify policy)" = "silenced:1 s=keepme priority:0"
dsp "hl.dsp.window.close()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
# the stored 400x300 is applied over the requested 500x300, then the
# 100000,100000 spot clamps to the bottom-right margin corner for THAT size
expect "far-off-screen seed: stored size applied, clamped to ($((MON_W-401)),$((MON_H-301)))" \
	"any(c['class']=='foot' and c['at']==[$((MON_W-401)),$((MON_H-301))] and c['size']==[400,300] for c in cs)"
# Close the seed. It clamps to the bottom-right corner, directly under the
# shade's column: the panel-bottom detector traces non-black rows down from
# the bar, so any window left there poisons every shade measurement taller
# than ~470px (the rim touches the window top and the run never breaks).
HF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$HF" ]] && dsp "hl.dsp.window.close({window=\"address:$HF\"})"; sleep 0.5
chk "hostile seed closed: no foot survives into the panel batteries" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
