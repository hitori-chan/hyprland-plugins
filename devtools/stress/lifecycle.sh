# ---- native input capture ------------------------------------------------
# The protocol is fed after plugin input listeners emit. A receiver that gets
# all three event classes proves the bar, shade, click, max, and snap gates
# did not cancel or swallow an event owned by an active capture session.
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

# ---- real-input storm ---------------------------------------------------
# Two probe windows make the post-storm assertion real: focus stormb by
# clicking it, run the storm (bar clicks only), then the post-storm click must
# still raise + focus storma — a stuck swallow would eat it and leave stormb
# as the mru-last client.
# foot maps asynchronously and the gate is busy here - wait for each class
# before the next spawn, or the two race for tile order and focus
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
chk "input storm: all 8 plugins alive" test "$(hq plugin list | grep -c Plugin)" = 8
chk "input storm: the final taglist click registered (ws 1)" test "$(ws)" = 1
read -r BAX BAY <<< "$(center_of storma)"; click_at "$BAX" "$BAY"
expect "post-storm click still raises + focuses (no stuck swallow)" \
	"cs[-1]['class']=='storma' if cs else False"
# the probes must not sit under the corpse battery's geometry assertions
for a in $(clients | python3 -c "import json,sys
print(' '.join(c['address'] for c in json.load(sys.stdin) if c['class'] in ('storma','stormb')))"
); do dsp "hl.dsp.window.close({window=\"address:$a\"})"; done; sleep 0.5

# ---- corpse guard (hyprclick) -------------------------------------------
# the tail of a fast double-click on a click-to-close window (Telegram's
# image viewer backdrop) lands after the unmap: it must be swallowed, not
# focus-and-raise whatever sat beneath (it flipped the live stack).
dsp "hl.dsp.exec_cmd('foot -a corpseA')"; sleep 1.6
# The placement store intentionally remembers the same class geometry. Move
# these two viewers apart after spawn so the dying viewer always has an
# exposed corner to receive the tail of the click burst.
dsp "hl.dsp.window.move({x=$((MON_W / 10)), y=$((MON_H / 6))})"; sleep 0.4
dsp "hl.dsp.exec_cmd('foot -a corpseB')"; sleep 1.6
dsp "hl.dsp.window.move({x=$((MON_W / 2)), y=$((MON_H / 8))})"; sleep 0.4
dsp "hl.dsp.exec_cmd('foot -a corpseB -F')"; sleep 1.6
# a point over corpseA that corpseB doesn't cover: the press that must die
P="$(clients | python3 -c "
import json,sys
cs=json.load(sys.stdin)
A=next(c for c in cs if c['class']=='corpseA')
B=next(c for c in cs if c['class']=='corpseB' and c['fullscreen']==0)
def inside(p,c): return c['at'][0]<=p[0]<c['at'][0]+c['size'][0] and c['at'][1]<=p[1]<c['at'][1]+c['size'][1]
pts=[(A['at'][0]+dx,A['at'][1]+dy) for dx in (30,A['size'][0]-30) for dy in (30,A['size'][1]-30)]
x,y=next(p for p in pts if not inside(p,B))
print(x,y)")"
V="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['fullscreen']==2))")"
# a 5-press burst spanning ~700ms: the tail outlives a fixed 400ms corpse,
# so this also asserts each swallowed press extends the gesture
( { printf "move ${P% *} ${P#* }\nsleep 30\n"; for i in 1 2 3 4 5; do printf "press 272\nsleep 40\nrelease 272\nsleep 110\n"; done; } | vp ) &
sleep 0.12; dsp "hl.dsp.window.close({window=\"address:$V\"})"
wait; sleep 0.9
expect "corpse guard: click burst through a dying viewer keeps the stack" \
	"cs[-1]['class']=='corpseB'"
chk "corpse guard: focus stayed with the viewer's app" active_window_class_is corpseB
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class'] in ('corpseA','corpseB')]"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
sleep 0.8

# ---- fullscreen tuck (hyprclick) ----------------------------------------
# clicking a fullscreen window tucks a floater flagged above it (the
# compositor's pointer-focus raise sets that flag mid-viewer) back behind —
# by FLAG ONLY: its stack position must survive the viewer, or it comes
# back buried under every maximized window (reproduced live).
dsp "hl.dsp.exec_cmd('foot -a tuckmax')"; sleep 1.6
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot -a tuckfloat')"; sleep 1.6
dsp "hl.dsp.window.move({x=500, y=300})"; dsp "hl.dsp.window.resize({x=500, y=300})"; sleep 0.4
dsp "hl.dsp.exec_cmd('foot -a tuckfs -F')"; sleep 1.6
TF="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['class']=='tuckfloat'))")"
V="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['fullscreen']==2))")"
dsp "hl.dsp.window.alter_zorder({mode=\"top\", window=\"address:$TF\"})"; sleep 0.3
# a point the floater does not cover: the raise-click must resolve to the
# fullscreen window and fire the tuck
P="$(clients | python3 -c "
import json,sys
F=next(c for c in json.load(sys.stdin) if c['class']=='tuckfloat')
x = F['at'][0]-80 if F['at'][0] >= 110 else F['at'][0]+F['size'][0]+80
print(int(x), int(F['at'][1]+50))")"
printf "move ${P% *} ${P#* }\nsleep 30\npress 272\nsleep 40\nrelease 272\nsleep 60\n" | vp
sleep 0.6
dsp "hl.dsp.window.close({window=\"address:$V\"})"; sleep 0.9
expect "fullscreen tuck: the flagged floater survives the viewer on top" \
	"[c['class'] for c in cs if c['class'].startswith('tuck')][-1]=='tuckfloat'"
chk "fullscreen tuck: floater focused after the viewer closes" active_window_class_is tuckfloat
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class'].startswith('tuck')]"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
sleep 0.8

# ---- config reload ------------------------------------------------------
# hyprbar and hyprnotify both re-resolve their disk-loaded icons from
# config.reloaded: the bar re-probes its icon dirs, drops every resolved
# texture, marks the live tray items dirty and warms again. Nothing may
# wedge, and the strip must still take a click afterwards.
#
# retarget FIRST, and not only for the signature: the Wayland backend can hand
# the instance a different window size before nested.lua's monitor line and a
# reload can apply another output size. vptr maps `move X Y` as X/extent, so
# every coordinate below is off by that ratio until the extent is re-read.
chk "reload: the config re-applies" hq_matches 'ok' reload
sleep 1.2
retarget || { echo "nested retarget FAILED after reload"; exit 1; }
chk "reload: all 8 plugins alive" test "$(hq plugin list | grep -c Plugin)" = 8
chk "reload: hyprnotify still answers" hq_matches '^center:' hyprnotify state
printf "move 59 13\nsleep 30\npress 272\nsleep 30\nrelease 272\nsleep 120\n" | vp; sleep 0.6
chk "reload: the strip still takes a click (tag 3)" test "$(ws)" = 3
printf "move 12 13\nsleep 30\npress 272\nsleep 30\nrelease 272\nsleep 120\n" | vp; sleep 0.6
chk "reload: and back on tag 1" test "$(ws)" = 1

# ---- log hygiene --------------------------------------------------------
chk "log clean (only known-benign lines)" bash -c \
	"! grep -iE 'error|assert|segv|abort' '$LOG' | grep -vE 'Creating the Error Overlay|xkbcomp' | grep -q ."

# ---- teardown -----------------------------------------------------------
# A mapped plugin cannot retain callbacks into its code after unload, but an
# external helper also cannot be allowed to hold compositor exit hostage.
# Start one helper through each owner (a wpctl readback that never returns and
# a sound helper that never finishes); require bounded shutdown, then remove
# the deliberately hung fixtures owned by this test.
: > "$STATE/hang-wpctl"
: > "$STATE/hang-sound"
dsp "hl.plugin.hyprosd.volume_up()"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i teardown 0 "" "hung sound" body 0 1 sound-name s gate 30000 >/dev/null 2>&1
for _ in $(seq 1 30); do
	[[ -s "$STATE/hang-wpctl.pid" && -s "$STATE/hang-sound.pid" ]] && break
	sleep 0.1
done
chk "teardown: hyprosd owns an active helper" test -s "$STATE/hang-wpctl.pid"
chk "teardown: hyprnotify owns an active helper" test -s "$STATE/hang-sound.pid"
NESTED_PID="$(validated_nested_pid 2>/dev/null)"
chk "teardown: nested compositor pid is known" test -n "$NESTED_PID"
kill_nested
for _ in $(seq 1 30); do
	! kill -0 "$NESTED_PID" 2>/dev/null && break
	sleep 0.1
done
if [[ -n "$NESTED_PID" ]] && ! kill -0 "$NESTED_PID" 2>/dev/null; then
	ok "teardown: active helpers do not block compositor exit"
else
	bad "teardown: active helpers do not block compositor exit"
	[[ -n "$NESTED_PID" ]] && kill -KILL "$NESTED_PID" 2>/dev/null || true
fi
cleanup_harness
echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
	echo "== stress: ALL $PASS CHECKS PASSED in ${SECONDS}s =="
	exit 0
else
	echo "== stress: $PASS passed, ${#FAILED[@]} FAILED in ${SECONDS}s =="
	printf '   - %s\n' "${FAILED[@]}"
	exit 1
fi
