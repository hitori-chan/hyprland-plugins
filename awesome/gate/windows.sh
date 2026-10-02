# awesome/gate/windows.sh — the windows module's behavior battery: spawn
# placement (the fresh store the migration fed), the CSD geometry battery,
# maximize, minimize/restore, the focus policy, the X11 ping battery, and
# the hostile state file.

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
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 1.2
chk "close storm: no stragglers" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
chk "tsv: exactly one foot row survives the coalesced save" test "$(grep -c $'\tfoot$' "$AW_SPOT")" = 1
chk "tsv: no temp-file debris" bash -c "! ls $AWSTATE/*.tmp 2>/dev/null | grep -q ."

# ---- spawn storm --------------------------------------------------------
for i in $(seq 1 8); do
	dsp "hl.dsp.exec_cmd('foot --window-size-pixels=$((400 + (i % 4) * 80))x$((250 + (i % 3) * 60))')" &
done; wait; sleep 2.5
expect "spawn storm: all 8 up, fully inside the workarea" \
	"sum(1 for c in cs if c['class']=='foot')==8 and all(c['at'][0]>=0 and c['at'][1]>=26 and c['at'][0]+c['size'][0]<=$MON_W and c['at'][1]+c['size'][1]<=$MON_H for c in cs if c['class']=='foot')"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 1.2

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
	bash -c "! grep -q $'\tfixwin\$' \"$AW_SPOT\""
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "fixed-size battery left no windows" \
	test "$(pyc "sum(1 for c in cs if c['class'] in ('fixwin','foot'))")" = 0

# ---- CSD geometry offset (discord-updater splash shape) ------------------
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 csdpin')"; sleep 2
expect "fixed CSD splash: box is the client-declared frame, not offset-inflated" \
	"any(c['class']=='csdpin' and c['floating'] and c['size']==[300,350] and abs(c['at'][0]-$(( (MON_W-300)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-350)/2 )))<=14 for c in cs)"
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
expect "per-axis-pinned CSD: pinned axis stays at the client frame" \
	"any(c['class']=='csdpinx' and c['floating'] and c['size']==[300,360] for c in cs)"
CX="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdpinx'), ''))")"
[[ -n "$CX" ]] && dsp "hl.dsp.window.close({window=\"address:$CX\"})"; sleep 1
chk "csd battery left no windows" test "$(pyc "sum(1 for c in cs if c['class'] in ('csdpin','csdresz','csdpinx'))")" = 0

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

# ---- hostile state file -------------------------------------------------
# The remembered-spawn store is the user-editable FRESH file now: a garbage
# line, an out-of-range number and an absurd spot must all be skipped or
# clamped, not fatal. The monolith's "fresh wins once live" migration means
# the probe writes the fresh path directly (the legacy file is a one-time
# source, not a live one).
kill_nested
rm -f -- "$AW_SPOT"
printf 'garbage\n42\n1e400\t0\t300\t200\tinffoot\n-100\t-100\t-50\t-50\tnegfoot\n100000\t100000\t400\t300\tfoot\n' > "$AW_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "hostile tsv: the monolith still loads" test "$(hq plugin list | grep -c Plugin)" = 1
dsp "hl.dsp.window.close()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
expect "far-off-screen seed: stored size applied, clamped to ($((MON_W-401)),$((MON_H-301)))" \
	"any(c['class']=='foot' and c['at']==[$((MON_W-401)),$((MON_H-301))] and c['size']==[400,300] for c in cs)"
HF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$HF" ]] && dsp "hl.dsp.window.close({window=\"address:$HF\"})"; sleep 0.5
chk "hostile seed closed: no foot survives into the panel batteries" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0

# ---- hostile LEGACY migration -------------------------------------------
# The one-time migration itself is hostile-file input: a garbage legacy with
# NO fresh file must filter row-by-row and still land a usable store.
kill_nested
rm -f -- "$AW_SPOT"
printf 'garbage\n1e400\t0\t300\t200\tinffoot\n-100\t-100\t-50\t-50\tnegfoot\n300\t200\t400\t300\tmigrfoot\n' > "$LEG_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "legacy migration: the hostile file filtered into the fresh store" \
	bash -c "! grep -q inffoot \"$AW_SPOT\" && ! grep -q negfoot \"$AW_SPOT\" && grep -q $'\tmigrfoot\$' \"$AW_SPOT\""
# fresh wins once live: a different legacy now must not clobber it
kill_nested
printf '999\t999\t100\t100\tfoot\n' > "$LEG_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "legacy migration: a second pass does not clobber the live store" \
	bash -c "! grep -q $'\tfoot\$' \"$AW_SPOT\" || grep -q $'\tmigrfoot\$' \"$AW_SPOT\""
kill_nested
rm -f -- "$AW_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }

# ---- X11 pings: urgency without focus steal (from focus.sh) ---------------
FOCUSTRAP="$REPO/devtools/focustrap"
if [[ ! -x "$FOCUSTRAP" ]]; then
	make -C "$REPO/devtools" focustrap >/dev/null 2>&1 || true
fi
if [[ ! -x "$FOCUSTRAP" ]]; then
	bad "focus: $FOCUSTRAP is unavailable"
else
	# The nested baseline IS the vanilla mode (nested.lua sets
	# focus_on_activate = true, the gate's permanent expectation — the old
	# inject/restore-at-runtime machinery is gone: a line left behind by an
	# interrupted run silently flipped every expectation, 2026-10-02). A
	# drifted config must FAIL the battery, not change what it means.
	chk "focus: the nested baseline is the vanilla gate (focus_on_activate on)" \
		grep -q "focus_on_activate = true" "$CFG"
	NPID_FOCUS="$(head -1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
	NDISP=""
	for p in $(pgrep -P "$NPID_FOCUS" 2>/dev/null); do
		c="$(tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null)"
		if [[ "$c" == Xwayland* ]]; then
			NDISP="$(sed -n 's/^Xwayland \([^ ]*\) .*/\1/p' <<<"$c")"
			break
		fi
	done
	if [[ -z "$NDISP" ]]; then
		bad "focus: nested X display is unavailable"
	else
		S2SOCK="$RUNDIR/$SIG/.socket2.sock"
		S2CAP_PY="$STATE/focus-s2.py"
		cat >"$S2CAP_PY" <<'PYEOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sys.argv[1])
except Exception:
    sys.exit(1)
s.sendall(b"ENABLE\n")
s.settimeout(0.4)
end = time.time() + float(sys.argv[2])
with open(sys.argv[3], "w") as out:
    while time.time() < end:
        try:
            d = s.recv(4096).decode("utf-8", "replace")
        except Exception:
            continue
        if not d:
            break
        for ln in d.splitlines():
            if ln.startswith("urgent>>") or ln.startswith("activewindow>>"):
                out.write(ln + "\n")
                out.flush()
PYEOF
		probe_addr() {
			clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='focustrap'), '')
print(a[2:] if a.startswith('0x') else a)"
		}
		focus_addr() {
			hq activewindow 2>/dev/null | awk '/^Window/ { print $2; exit }'
		}
		click_center() {
			clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class']=='$1'), None)
print(int(c['at'][0]+c['size'][0]/2), int(c['at'][1]+c['size'][1]/2)) if c else (0,0)" \
				| { read -r cx cy; printf "move %s %s\nsleep 50\npress 272\nsleep 50\nrelease 272\nsleep 50\n" "$cx" "$cy"; } | vp
		}
		run_ping_mode() {
			local mode=$1
			local cap="$STATE/focus-$mode.cap" lpid="" probe="" paddr="" faddr=""
			DISPLAY="$NDISP" "$FOCUSTRAP" "$mode" 5 16 >/dev/null 2>&1 &
			probe=$!
			sleep 1.8
			paddr="$(probe_addr)"
			if [[ -z "$paddr" ]]; then
				bad "focus($mode): probe window never mapped"
				kill "$probe" 2>/dev/null
				return
			fi
			ok "focus($mode): probe mapped"
			click_center foot
			faddr="$(focus_addr)"
			chk "focus($mode): foot holds focus after the map" test -n "$faddr"
			: >"$cap"
			python3 "$S2CAP_PY" "$S2SOCK" 14 "$cap" &
			lpid=$!
			sleep 9
			chk "focus($mode): the ping did not move the keyboard focus" \
				test "$(focus_addr)" = "$faddr"
			click_center focustrap
			chk "focus($mode): clicking the urgent probe focuses it" \
				test "$(focus_addr)" = "$paddr"
			kill "$probe" 2>/dev/null
			wait "$lpid" 2>/dev/null
			if [[ "$mode" == "map" ]]; then
				chk "focus(map): a bare map raises no urgency" \
					bash -c "! grep -q 'urgent>>' '$cap'"
			else
				chk "focus($mode): the ping posted an urgent event for the probe window" \
					grep -q "urgent>>$paddr" "$cap"
			fi
		}
		run_ping_mode map
		run_ping_mode attention
		run_ping_mode activate
		# activation of a MINIMIZED window: the monolith restores it (the
		# state machine's urgent hop) — the old bar's deferred restore. The
		# probe pings its OWN X11 toplevel 14s after the map; the probe is
		# focused then minimized in the meantime, so the ping is the test.
		DISPLAY="$NDISP" "$FOCUSTRAP" activate 2 14 >/dev/null 2>&1 &
		FP=$!
		sleep 1.8
		click_center focustrap
		dsp "hl.plugin.awesome.minimize()"; sleep 0.8
		chk "urgent-restore: the probe is minimized" \
			test "$(pyc "any(c['class']=='focustrap' and c['hidden'] for c in cs)")" = 1
		sleep 13
		chk "urgent-restore: activating a minimized window restores it" \
			test "$(pyc "any(c['class']=='focustrap' and not c['hidden'] for c in cs)")" = 1
		kill "$FP" 2>/dev/null
	fi
	# ---- vanilla activation (Wayland xdg-activation, the token-validated path)
	# (a) VISIBLE ask: the tray-return burst — a Wayland map (which takes the
	#     new-map initial focus, awesome's "rules" context) followed
	#     immediately by an xdg-activation ask. The ask lands on a window
	#     that already holds focus: vanilla neither moves the focus nor
	#     marks the focused client (permissions.urgent: c ~= client.focus).
	#     Wayland on purpose: an X11 ping's arrival is offset by the Xwayland
	#     pipeline lag and voids the sub-second burst; the Wayland burst
	#     lands in milliseconds.
	# (b) NOT-VISIBLE ask: the same burst with a delay — the battery moves
	#     the compositor to another workspace in between, so the ask arrives
	#     at a window whose workspace is off-monitor: permissions.activate's
	#     isvisible branch — urgency, no focus, no workspace switch.
	ACTWIN="$REPO/devtools/activatewin"
	[[ -x "$ACTWIN" ]] || make -C "$REPO/devtools" activatewin >/dev/null 2>&1
	if [[ -n "$NDISP" && -x "$ACTWIN" ]]; then
		# let the killed X11 probe fully leave the window list first: its
		# destroy is async through Xwayland, and a click (or a new map's
		# focus fallback) racing the dying surface lands on dead geometry
		# (2026-10-02 battery repro). An empty S1 response is a transport
		# hiccup, not "no windows": keep waiting on it.
		for _ in $(seq 1 20); do
			CL="$(clients 2>/dev/null)"
			if [[ -n "$CL" ]]; then
				echo "$CL" | grep -q '"class": *"focustrap"' || break
			fi
			sleep 0.25
		done
		# a mid-battery relaunch (the harness's FALLBACK recovery) leaves a
		# bare nested: the foot this section spawned is gone with the old
		# instance. Re-establish the precondition instead of clicking empty
		# air (2026-10-02: the check read clients=[] and failed for that
		# reason).
		clients 2>/dev/null | grep -q '"class": *"foot"' \
			|| { dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2.5; }

		# (a) the visible burst
		click_center foot
		# the click's focus round-trip is one compositor loop turn; give
		# it a bounded wait instead of a single immediate read
		RF=""
		for _ in $(seq 1 8); do RF="$(focus_addr)"; [[ -n "$RF" ]] && break; sleep 0.25; done
		if [[ -z "$RF" ]]; then
			bad "vanilla-visible: no pre-map focus to click against (the click missed foot)"
			echo "  diag raw clients: $(clients 2>/dev/null | head -c 1200)" >&2
		else
			: >"$STATE/vanvis.cap"
			python3 "$S2CAP_PY" "$S2SOCK" 8 "$STATE/vanvis.cap" &
			RLPID=$!
			env WAYLAND_DISPLAY="$WL" "$ACTWIN" 8 >"$STATE/vanvis.win.log" 2>&1 &
			RP=$!
			for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
			RPAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
			sleep 1.5 # the ask lands in milliseconds; the settle covers the event round-trip
		chk "vanilla-visible: the map focus holds after the ask" \
			test "$(focus_addr)" = "$RPAD"
		chk "vanilla-visible: the focused window is not marked urgent" \
			bash -c "! grep -q 'urgent>>$RPAD' '$STATE/vanvis.cap'"
			# one line for the log either way: a FAIL must say what the client
			# did (mapped? asked?), what the capture saw, and where focus ended
			echo "vanvis: pre-map='$RF' post='$(focus_addr)' probe='$RPAD' urgent-lines=$(grep -c 'urgent>>' "$STATE/vanvis.cap" 2>/dev/null) client=[$(cat "$STATE/vanvis.win.log" 2>/dev/null | tr '\n' '|')]" >&2
			kill "$RP" 2>/dev/null
			wait "$RLPID" 2>/dev/null
		fi

		# (b) the not-visible ask: the probe maps on the current workspace and
		# takes the map focus; the battery then moves to workspace 2, taking
		# the probe's workspace off the monitor, and the delayed ask (6s in)
		# arrives at a not-visible window
		: >"$STATE/vannv.cap"
		python3 "$S2CAP_PY" "$S2SOCK" 13 "$STATE/vannv.cap" &
		NVLPID=$!
		env WAYLAND_DISPLAY="$WL" "$ACTWIN" 13 6 >"$STATE/vannv.win.log" 2>&1 &
		NP=$!
		for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
		dsp "hl.dsp.focus({workspace=\"2\"})"; sleep 0.8
		# the fixture prints when the delayed ask went out; wait for it,
		# bounded, instead of a fixed sleep racing the dispatch loop
		for _ in $(seq 1 16); do grep -q "sent activation" "$STATE/vannv.win.log" 2>/dev/null && break; sleep 0.5; done
		sleep 1
		NPAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
		chk "vanilla-notvisible: the ask posted an urgent event for the probe" \
			grep -q "urgent>>$NPAD" "$STATE/vannv.cap"
		chk "vanilla-notvisible: the focus did not follow the ask" \
			bash -c "test \"$(focus_addr)\" != \"$NPAD\""
		dsp "hl.dsp.focus({workspace=\"1\"})" # the probe keeps its mark on ws 1
		kill "$NP" 2>/dev/null
		wait "$NVLPID" 2>/dev/null
		echo "vannv: probe='$NPAD' focus='$(focus_addr)' urgent-lines=$(grep -c 'urgent>>' "$STATE/vannv.cap" 2>/dev/null) client=[$(cat "$STATE/vannv.win.log" 2>/dev/null | tr '\n' '|')]" >&2
	fi
	# the (b) probe may still be settling out; make sure the view is back on
	# workspace 1 before the foot-cleanup trailer
	dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 0.5
fi
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "windows: its foot is closed before the geometry batteries" \
	test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
