# awesome/gate/pipeline.sh — the input pipeline's contract: native input-capture
# pass-through (the receiver must see all three event classes uncancelled),
# the real-input storm (a stuck swallow eats the post-storm click at any
# volume), and the surface-ownership precedence (a bar-owned surface in front
# of a window does not let the window policy act on its clicks).
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- native input capture ------------------------------------------------
# The protocol is fed after plugin input listeners emit. A receiver that gets
# all three event classes proves the supervisor's dispatch did not cancel or
# swallow an event owned by an active capture session.
CAPTURE_READY=0
WAYLAND_DISPLAY="$WL" timeout 12s "$REPO/devtools/input-capture" "$MON_W" "$MON_H" >"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!
for _ in $(seq 1 50); do
	if grep -qx 'READY' "$CAPTURE_LOG" 2>/dev/null; then
		CAPTURE_READY=1
		break
	fi
	if ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
		break
	fi
	sleep 0.1
done
chk "input capture: receiver reached READY" test "$CAPTURE_READY" = 1
if [[ $CAPTURE_READY == 1 ]]; then
	{
		echo "move $((MON_W / 2)) $((MON_H / 2))"
		echo "sleep 60"
		# Absolute motion is a warp and does not cross input-capture barriers;
		# use native relative motion for the edge crossing.
		echo "rel 0 -$((MON_H / 2 + 40))"
		echo "sleep 120"
		echo "move $((MON_W / 2 + 40)) $((MON_H / 2))"
		echo "sleep 80"
		echo "press 272"
		echo "sleep 40"
		echo "release 272"
		echo "sleep 80"
	} | vp
	# Creating vkbd after activation replaces the compositor's active keymap;
	# this exercises the fork's EIS keyboard-device restart while captured.
	printf 'tap a\nsleep 100\n' | vk
	wait "$CAPTURE_PID"; CAPTURE_STATUS=$?
	CAPTURE_PID=""
	if ! chk "input capture: motion, button, and key events reached EIS" test "$CAPTURE_STATUS" = 0; then
		# the receiver's timeout line says WHICH event class never arrived
		sed 's/^/  input-capture: /' "$CAPTURE_LOG" 2>/dev/null
	fi
else
	stop_capture
fi
sleep 0.5

# ---- real-input storm ----------------------------------------------------
# Two probe windows make the post-storm assertion real: focus stormb by
# clicking it, run the storm (bar clicks only), then the post-storm click must
# still raise + focus storma — a stuck swallow would eat it and leave stormb
# as the mru-last client.
wait_class() { for _ in $(seq 1 40); do [[ "$(pyc "any(c['class']=='$1' for c in cs)")" = 1 ]] && return 0; sleep 0.25; done; return 1; }
center_of() { clients | python3 -c "
import json,sys
c = next(c for c in json.load(sys.stdin) if c['class'] == '$1')
print(c['at'][0] + c['size'][0] // 2, c['at'][1] + c['size'][1] // 2)"; }
click_at() { printf 'move %s %s\nsleep 50\npress 272\nsleep 40\nrelease 272\nsleep 100\n' "$1" "$2" | vp; sleep 0.8; }
dsp "hl.dsp.exec_cmd('foot -a storma --window-size-pixels=300x200')"
wait_class storma
dsp "hl.dsp.exec_cmd('foot -a stormb --window-size-pixels=300x200')"
wait_class stormb
sleep 0.5
read -r BBX BBY <<< "$(center_of stormb)"; click_at "$BBX" "$BBY"
chk "storm probes up: stormb focused by its own click" test "$(pyc "cs[-1]['class']=='stormb' if cs else False")" = 1
{
	# the burst size is not the property under test — a stuck swallow eats
	# the post-storm click at any volume; 30/8/5 keeps the same event mix
	for i in $(seq 1 30); do echo "move $(( (i * 97) % MON_W )) $(( 30 + (i * 61) % (MON_H - 40) ))"; echo "sleep 10"; done
	for i in $(seq 1 8); do echo "move 500 13"; echo "sleep 15"; echo "scroll 0 1"; echo "sleep 25"; done
	for i in $(seq 1 5); do
		echo "move 34 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
		echo "move 59 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
	done
	echo "move 12 13"; echo "sleep 30"; echo "press 272"; echo "sleep 30"; echo "release 272"; echo "sleep 100"
} | vp
sleep 1
chk "input storm: the monolith is alive" test "$(hq plugin list | grep -c Plugin)" = 1
chk "input storm: the final taglist click registered (ws 1)" test "$(ws)" = 1
read -r BAX BAY <<< "$(center_of storma)"; click_at "$BAX" "$BAY"
expect "post-storm click still raises + focuses (no stuck swallow)" \
	"cs[-1]['class']=='storma' if cs else False"

# ---- surface ownership: the bar's surfaces beat the window policy ----------
# The shade sits over the focused window. A click INSIDE the shade (on a row
# with no card there, i.e. the empty body area below the rows) must be the
# shade's (close it) and must not reach the window beneath it as a focus
# raise — and the window's focus must survive the whole exchange.
dsp "hl.dsp.exec_cmd('foot -a prec --window-size-pixels=500x300')"
wait_class prec
PFX="$(clients | python3 -c "
import json,sys
c = next(c for c in json.load(sys.stdin) if c['class']=='prec')
print(c['at'][0] + c['size'][0] // 2, c['at'][1] + c['size'][1] // 2)")"
click_at ${PFX% *} ${PFX#* }
chk "ownership: prec focused by its own click" test "$(pyc "cs[-1]['class']=='prec' if cs else False")" = 1
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"ownership\" body')"; sleep 1
hq awesome center >/dev/null; sleep 0.7
chk "ownership: the shade is open over the focused window" test "$(st)" = "center:1 live:1 dnd:0"
# an empty strip of the shade below the single row: 8 px above the measured
# panel bottom is inside the panel and below the row's hit box (44..103)
capture_nested "$STATE/own-panel.png"
PB_OW="$(panel_bottom "$STATE/own-panel.png")"
click $(( PANEL_X + 40 )) $(( PB_OW - 8 )) 272
chk "ownership: the shade took the click (it closed)" test "$(st)" = "center:0 live:1 dnd:0"
chk "ownership: the window beneath kept its focus" test "$(pyc "cs[-1]['class']=='prec' if cs else False")" = 1
hq awesome clear >/dev/null; sleep 0.5
for a in $(clients | python3 -c "import json,sys
print(' '.join(c['address'] for c in json.load(sys.stdin) if c['class'] in ('storma','stormb','prec')))"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})"
done; sleep 0.5
chk "pipeline: its windows are closed before the teardown battery" \
	test "$(pyc "sum(1 for c in cs if c['class'] in ('storma','stormb','prec'))")" = 0
