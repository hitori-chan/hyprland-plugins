# awesome/gate/focus.sh — the focus-policy battery: the X11 ping path
# (urgency without focus steal, from focus.sh) and the Wayland
# xdg-activation (token-validated) path, in the nested's attention-only
# baseline (focus_on_activate at the fork default — the user's live mode).
# ---- X11 pings: urgency without focus steal (from focus.sh) ---------------
FOCUSTRAP="$REPO/devtools/focustrap"
if [[ ! -x "$FOCUSTRAP" ]]; then
	make -C "$REPO/devtools" focustrap >/dev/null 2>&1 || true
fi
if [[ ! -x "$FOCUSTRAP" ]]; then
	bad "focus: $FOCUSTRAP is unavailable"
else
	# The nested baseline IS the user's live mode (nested.lua leaves
	# focus_on_activate at the fork default, false — attention-only
	# activation, the mode the user runs). The old inject/restore-at-runtime
	# machinery is gone: a line left behind by an interrupted run silently
	# flipped every expectation (2026-10-02). A drifted config must FAIL the
	# battery, not change what it means.
	chk "focus: the nested baseline is the attention-only default (no focus_on_activate line)" \
		bash -c "! grep -q 'focus_on_activate' '$CFG'"
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
		# click_point <class>: a point INSIDE the window that no other window
		# covers, or "0 0" when the class is missing or fully covered. The old
		# center-only click silently landed on whatever covered the center
		# (the X11 probe maps workarea-sized; after the hostile batteries a
		# fresh foot sat over its center) and the focus check failed for the
		# wrong reason. The sample grid keeps the search bounded.
		click_point() {
			clients | python3 -c "
import json,sys
cs = json.load(sys.stdin)
t = next((c for c in cs if c['class']==sys.argv[1]), None)
if t is None:
    print('0 0'); raise SystemExit
x, y = t['at']; w, h = t['size']
def inside(p, c):
    return c['at'][0] <= p[0] < c['at'][0]+c['size'][0] and c['at'][1] <= p[1] < c['at'][1]+c['size'][1]
pts = [(x+max(1,int(w*f/100)), y+max(1,int(h*g/100))) for f in (5,25,50,75,95) for g in (5,25,50,75,95)]
for p in pts:
    if all(not inside(p, c) for c in cs if c['address'] != t['address']):
        print(*p); raise SystemExit
print(x+w//2, y+h//2)" "$1"
		}
		click_xy() { # click_xy <x> <y>
			printf "move %s %s\nsleep 50\npress 272\nsleep 50\nrelease 272\nsleep 50\n" "$1" "$2" | vp
		}
		focus_class() { # focus_class <class>: click a reachable point; 1 = nothing to click
			read -r _cx _cy < <(click_point "$1")
			[[ "$_cx" == "0" && "$_cy" == "0" ]] && return 1
			click_xy "$_cx" "$_cy"
			return 0
		}
		geomdump() { # the geometry a click decision was made against
			clients | python3 -c "
import json,sys
print(' '.join(f\"{c['class']}@{c['at'][0]},{c['at'][1]}:{c['size'][0]}x{c['size'][1]}\" for c in json.load(sys.stdin)))" 2>/dev/null
		}
		touch_probe() { # touch_probe <class>: a keyboard event for the window
			# (must hold keyboard focus: in every case it holds the map focus).
			# The new upstream's xdg-activation token path validates the
			# client's set_serial against ITS OWN seat events (anti-spoofing):
			# a client that never saw a seat event has no serial to spend, the
			# token comes back empty and the ask is void. A key routed to the
			# probe hands the fixture a serial with no pointer motion (no
			# follow-focus, no position dependence) and no press (no click).
			clients | python3 -c "
import json,sys
print(1 if any(c['class']=='$1' for c in json.load(sys.stdin)) else 0)" \
			| { read -r up; [[ "$up" == 1 ]] && printf 'tap 89\nsleep 200\n' | vk; }
		}
		wait_gone() { # wait_gone <class>: bounded wait for no client of the class
			for _ in $(seq 1 24); do
				clients 2>/dev/null | grep -q "\"class\": *\"$1\"" || return 0
				sleep 0.25
			done
			return 1
		}
		run_ping_mode() {
			local mode=$1
			local cap="$STATE/focus-$mode.cap" trig="$STATE/focus-$mode.trigger"
			local lpid="" probe="" paddr="" faddr=""
			rm -f "$trig"
			# Trigger-driven ping: the fixture fires the moment $trig appears —
			# the battery touches it only once focus is demonstrably OFF the
			# probe, so the attention-only (GATED) path is guaranteed at ping
			# time. A fixed delay races the battery's own clicks under
			# compositor stalls (the ping lands on the just-refocused probe and
		# is a no-op — 2026-10-03). Hold 18 outlives the late-event poll.
			DISPLAY="$NDISP" "$FOCUSTRAP" "$mode" "$trig" 18 >/dev/null 2>&1 &
			probe=$!
			sleep 1.8
			paddr="$(probe_addr)"
			if [[ -z "$paddr" ]]; then
				bad "focus($mode): probe window never mapped"
				kill "$probe" 2>/dev/null
				return
			fi
			ok "focus($mode): probe mapped"
			# S2 streams only from connect time: capture from now, so the
			# trigger-firing event below can never land in a not-yet-open file
			python3 "$S2CAP_PY" "$S2SOCK" 30 "$cap" &
			lpid=$!
			if ! focus_class foot; then
				bad "focus($mode): no reachable click point on foot: $(geomdump)"
				kill "$probe" 2>/dev/null
				wait "$lpid" 2>/dev/null
				return
			fi
			faddr="$(focus_addr)"
			# the probe takes the map focus, so the click must demonstrably
			# land on foot: with the probe focused the ping below early-returns
			# (an activation request on the focused window is a no-op) and the
			# urgent check fails for the wrong reason
			if [[ -n "$faddr" && "$faddr" != "$paddr" ]]; then
				ok "focus($mode): the click moved focus off the probe to foot"
			else
				bad "focus($mode): the click moved focus off the probe to foot (focus=[$faddr] want!=[$paddr])"
				geomdump
				kill "$probe" 2>/dev/null
				wait "$lpid" 2>/dev/null
				return
			fi
			if [[ "$mode" != "map" ]]; then
				# fire the ping with focus demonstrably on foot, then settle
				# past the Xwayland pipeline lag before asserting the focus held
				touch "$trig"
				sleep 2.5
				chk "focus($mode): the ping did not move the keyboard focus" \
					test "$(focus_addr)" = "$faddr"
			fi
			if ! focus_class focustrap; then
				bad "focus($mode): the probe is fully covered, no reachable point: $(geomdump)"
				kill "$probe" 2>/dev/null
				wait "$lpid" 2>/dev/null
				return
			fi
			if [[ "$(focus_addr)" == "$paddr" ]]; then
				ok "focus($mode): clicking the urgent probe focuses it"
			else
				bad "focus($mode): clicking the urgent probe focuses it (focus=[$(focus_addr)] want=[$paddr])"
				geomdump
			fi
			if [[ "$mode" == "map" ]]; then
				# no ping: a settle past where an event would have landed, then
				# the absence is the assertion
				sleep 2
				kill "$probe" 2>/dev/null
				wait "$lpid" 2>/dev/null
				chk "focus(map): a bare map raises no urgency" \
					bash -c "! grep -q 'urgent>>' '$cap'"
			else
				# wait for the event, bounded: the X pipeline lag is unbounded
				# from the outside, so poll the capture instead of betting a
				# fixed window outlives it. The probe stays UP until the event
				# is in hand — a dead X client's queued ClientMessage may be
				# dropped, which would void the ping itself
				local got=0
				for _ in $(seq 1 24); do
					grep -q "urgent>>$paddr" "$cap" 2>/dev/null && { got=1; break; }
					sleep 0.5
				done
				kill "$probe" 2>/dev/null
				wait "$lpid" 2>/dev/null
				chk "focus($mode): the ping posted an urgent event for the probe window" \
					bash -c "test '$got' = 1"
			fi
		}
		# foot is the ping's focus target; the geometry batteries closed
		# theirs, so make sure a window to click is up (a click_center with
		# no foot clicks (0,0), the bar, and the probe keeps the map focus)
		clients 2>/dev/null | grep -q '"class": *"foot"' \
			|| { dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2.5; }

		run_ping_mode map
		run_ping_mode attention
		run_ping_mode activate
		# attention on a MINIMIZED window, gate off: the ask is demoted to the
		# urgency mark, the window STAYS minimized — the chip's tint is the
		# whole answer; a chip click or Mod+Ctrl+N restores. (Gate on, the
		# plugin's urgent hop performs the restore instead — a config flip,
		# not a battery case.) The probe pings its OWN X11 toplevel once the
		# battery fires the trigger (below, after the minimize); the probe is
		# focused then minimized first, so the ping is the test.
		FMTRIG="$STATE/focus-min.trigger"; rm -f "$FMTRIG"
		: >"$STATE/focus-min.cap"
		python3 "$S2CAP_PY" "$S2SOCK" 25 "$STATE/focus-min.cap" &
		FMPL=$!
		DISPLAY="$NDISP" "$FOCUSTRAP" activate "$FMTRIG" 15 >/dev/null 2>&1 &
		FP=$!
		sleep 1.8
		PMAPADDR="$(probe_addr)"
		if [[ -z "$PMAPADDR" ]]; then
			bad "urgent-min: probe window never mapped"
		else
			focus_class focustrap || bad "urgent-min: no reachable click point on the probe: $(geomdump)"
			dsp "hl.plugin.awesome.minimize()"; sleep 0.8
			chk "urgent-min: the probe is minimized" \
				test "$(pyc "any(c['class']=='focustrap' and c['hidden'] for c in cs)")" = 1
			touch "$FMTRIG"
			# poll the event (it doubles as proof the ping was processed); the
			# probe stays up until then — a dead X client's queued ping may be
			# dropped, voiding the test. The 12s worst case still lands before
			# the probe's map+4+12s lifetime
			GOTMIN=0
			for _ in $(seq 1 24); do
				grep -q "urgent>>$PMAPADDR" "$STATE/focus-min.cap" 2>/dev/null && { GOTMIN=1; break; }
				sleep 0.5
			done
			chk "urgent-min: the ask did not restore the minimized window (attention-only mode)" \
				test "$(pyc "any(c['class']=='focustrap' and c['hidden'] for c in cs)")" = 1
			chk "urgent-min: the ask on the minimized window posted an urgent event" \
				bash -c "test '$GOTMIN' = 1"
		fi
		kill "$FP" 2>/dev/null
		wait "$FMPL" 2>/dev/null
	fi
	# ---- Wayland xdg-activation (the token-validated path), attention-only
	#     default (focus_on_activate at the fork default, the user's live
	#     mode — a message arriving in a background app must not steal):
	# (a)  VISIBLE ask on the FOCUSED window: the tray-return burst — a
	#      Wayland map (which takes the new-map initial focus) followed
	#      immediately by an xdg-activation ask. A focused, visible window is
	#      being looked at: the ask is a no-op, in both gate modes (vanilla's
	#      permissions.urgent never marks the focused client).
	# (a') VISIBLE ask on a NOT-FOCUSED window — the Telegram case: the probe
	#      maps (map focus), the battery clicks foot, and the delayed ask
	#      arrives at the visible, unfocused probe: urgency mark, focus stays
	#      on foot. With focus_on_activate on the same ask would take the
	#      focus (vanilla/KWin-token behavior) — a config flip, not a battery
	#      case.
	# (b)  NOT-VISIBLE ask: the delayed ask arrives after the battery moved to
	#      another workspace: permissions.activate's isvisible branch —
	#      urgency, no focus, no workspace switch (both gate modes).
	# (d)  jumpto: the Mod+U chord (hl.dsp.focus urgent_or_last) — the urgent
	#      window wins over the last-window fallback and the view follows it
	#      to its workspace (awesome's awful.client.urgent.jumpto).
	# Wayland on purpose: an X11 ping's arrival is offset by the Xwayland
	# pipeline lag and voids the sub-second burst; the Wayland burst lands in
	# milliseconds.
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

		# (a) the visible ask on the focused window
		focus_class foot
		# the click's focus round-trip is one compositor loop turn; give
		# it a bounded wait instead of a single immediate read
		RF=""
		for _ in $(seq 1 8); do RF="$(focus_addr)"; [[ -n "$RF" ]] && break; sleep 0.25; done
		if [[ -z "$RF" ]]; then
			bad "ask-focused: no pre-map focus to click against (the click missed foot)"
			echo "  diag raw clients: $(clients 2>/dev/null | head -c 1200)" >&2
		else
			: >"$STATE/askfocus.cap"
			python3 "$S2CAP_PY" "$S2SOCK" 8 "$STATE/askfocus.cap" &
			RLPID=$!
			env WAYLAND_DISPLAY="$WL" "$ACTWIN" 8 >"$STATE/askfocus.win.log" 2>&1 &
			RP=$!
			for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
			RPAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
			sleep 1.5 # the ask lands in milliseconds; the settle covers the event round-trip
			chk "ask-focused: the map focus holds after the ask" \
				test "$(focus_addr)" = "$RPAD"
			chk "ask-focused: the focused window is not marked urgent" \
				bash -c "! grep -q 'urgent>>$RPAD' '$STATE/askfocus.cap'"
				# one line for the log either way: a FAIL must say what the client
				# did (mapped? asked?), what the capture saw, and where focus ended
				echo "askfocus: pre-map='$RF' post='$(focus_addr)' probe='$RPAD' urgent-lines=$(grep -c 'urgent>>' "$STATE/askfocus.cap" 2>/dev/null) client=[$(cat "$STATE/askfocus.win.log" 2>/dev/null | tr '\n' '|')]" >&2
				kill "$RP" 2>/dev/null
				wait "$RLPID" 2>/dev/null
			wait_gone activatewin 2>/dev/null || true
		fi

		# (a') the visible ask on a NOT-focused window (the Telegram case):
		# the probe maps (map focus), the battery focuses foot PROGRAMMATICALLY
		# and the delayed ask (2s in) arrives at the visible, unfocused probe.
		# No vptr click here: a click's focus rides on follow-mouse, and any
		# later pointer event in the nested re-takes whatever it lands on,
		# racing the delayed ask (2026-10-02 battery flake). With no pointer
		# motion between the focus and the ask, the check is deterministic.
		: >"$STATE/askunfoc.cap"
		python3 "$S2CAP_PY" "$S2SOCK" 25 "$STATE/askunfoc.cap" &
		AFPL=$!
		env WAYLAND_DISPLAY="$WL" "$ACTWIN" 18 3 >"$STATE/askunfoc.win.log" 2>&1 &
		AP=$!
		for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
		APAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
		FAD="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
		# the tap precedes the programmatic focus: the fixture holds a
		# serial to spend when the delayed ask lands, and no pointer event
		# races the focus between it and the ask. The ask's delay (3s) must
		# outrun the tap's overhead plus the focus dispatch, or it lands
		# while the probe still holds the map focus and the ask is a
		# focused-visible no-op (2026-10-03 flake: the 2s ask raced the
		# ~1.8s focus land and won half the runs)
		touch_probe activatewin
		dsp "hl.dsp.focus({window=\"address:$FAD\"})"
		AF=""; for _ in $(seq 1 8); do AF="$(focus_addr)"; [[ -n "$AF" && "$AF" != "$APAD" ]] && break; sleep 0.25; done
		for _ in $(seq 1 8); do grep -q "sent activation" "$STATE/askunfoc.win.log" 2>/dev/null && break; sleep 0.5; done
		sleep 1
		chk "ask-unfocused: the ask did not steal focus from the front window" \
			test "$(focus_addr)" = "$AF"
		# poll the event instead of betting a fixed capture window outlives
		# the pipeline lag; the probe holds 18s so the 9s worst case lands
		# while it is still a live client
		GOTU=0
		for _ in $(seq 1 18); do
			grep -q "urgent>>$APAD" "$STATE/askunfoc.cap" 2>/dev/null && { GOTU=1; break; }
			sleep 0.5
			done
		chk "ask-unfocused: the ask posted an urgent event for the probe" \
			bash -c "test '$GOTU' = 1"
		echo "askunfoc: foot='$AF' post='$(focus_addr)' probe='$APAD' urgent-lines=$(grep -c 'urgent>>' "$STATE/askunfoc.cap" 2>/dev/null) client=[$(cat "$STATE/askunfoc.win.log" 2>/dev/null | tr '\n' '|')]" >&2
		kill "$AP" 2>/dev/null
		wait "$AFPL" 2>/dev/null
		wait_gone activatewin 2>/dev/null || true

		# (b) the not-visible ask: the probe maps on the current workspace and
		# takes the map focus; the battery then moves to workspace 2, taking
		# the probe's workspace off the monitor, and the delayed ask (6s in)
		# arrives at a not-visible window
		: >"$STATE/asknv.cap"
		python3 "$S2CAP_PY" "$S2SOCK" 25 "$STATE/asknv.cap" &
		NVLPID=$!
		env WAYLAND_DISPLAY="$WL" "$ACTWIN" 18 6 >"$STATE/asknv.win.log" 2>&1 &
		NP=$!
		for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
		# the tap lands while the probe is on the visible workspace;
		# the serial outlives the workspace switch (the seat container
		# keeps it until the token spends it)
		touch_probe activatewin
		dsp "hl.dsp.focus({workspace=\"2\"})"; sleep 0.8
		# the fixture prints when the delayed ask went out; wait for it,
		# bounded, instead of a fixed sleep racing the dispatch loop
		for _ in $(seq 1 16); do grep -q "sent activation" "$STATE/asknv.win.log" 2>/dev/null && break; sleep 0.5; done
		sleep 1
		NPAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
		GOTNV=0
		for _ in $(seq 1 12); do
			grep -q "urgent>>$NPAD" "$STATE/asknv.cap" 2>/dev/null && { GOTNV=1; break; }
			sleep 0.5
			done
		chk "ask-notvisible: the ask posted an urgent event for the probe" \
			bash -c "test '$GOTNV' = 1"
		chk "ask-notvisible: the focus did not follow the ask" \
			bash -c "test \"$(focus_addr)\" != \"$NPAD\""

		# (d) jumpto from the other workspace: the urgent window must beat the
		# last-window fallback (foot, focused before the probe's map) and the
		# view must follow to ws 1
		dsp "hl.dsp.focus({urgent_or_last=true})"; sleep 1.2
		chk "jumpto: the urgent window wins over the last-window fallback" \
			test "$(focus_addr)" = "$NPAD"
		chk "jumpto: the view followed to the urgent window's workspace" \
			test "$(ws)" = 1
		echo "asknv: probe='$NPAD' focus='$(focus_addr)' urgent-lines=$(grep -c 'urgent>>' "$STATE/asknv.cap" 2>/dev/null) client=[$(cat "$STATE/asknv.win.log" 2>/dev/null | tr '\n' '|')]" >&2
		kill "$NP" 2>/dev/null
		wait "$NVLPID" 2>/dev/null
		wait_gone activatewin 2>/dev/null || true

		# (n) the rejected ask: upstream's token path validates set_serial
		# against the client's own seat events (anti-spoofing). NO_SERIAL
		# skips the set_serial; the token comes back empty and the ask is
		# void. The probe is unfocused, so a PROCESSED ask would demote to
		# the urgency mark (attention-only) — no urgent line is the proof
		# the token was rejected, and focus must stay on foot either way.
		: >"$STATE/asknos.cap"
		python3 "$S2CAP_PY" "$S2SOCK" 6 "$STATE/asknos.cap" &
		NSLPID=$!
		env NO_SERIAL=1 WAYLAND_DISPLAY="$WL" "$ACTWIN" 6 1 >"$STATE/asknos.win.log" 2>&1 &
		NSP=$!
		for _ in $(seq 1 24); do clients 2>/dev/null | grep -q '"class": *"activatewin"' && break; sleep 0.25; done
		NSAD="$(clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='activatewin'), '')
print(a[2:] if a.startswith('0x') else a)")"
		NSFAD="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
		dsp "hl.dsp.focus({window=\"address:$NSFAD\"})"
		NSAF=""; for _ in $(seq 1 8); do NSAF="$(focus_addr)"; [[ -n "$NSAF" && "$NSAF" != "$NSAD" ]] && break; sleep 0.25; done
		for _ in $(seq 1 8); do grep -q "sent activation" "$STATE/asknos.win.log" 2>/dev/null && break; sleep 0.5; done
		sleep 1
		chk "ask-noserial: the unauthenticated ask raises no urgency" \
			bash -c "! grep -q 'urgent>>' '$STATE/asknos.cap'"
		chk "ask-noserial: the unauthenticated ask takes no focus" \
			test "$(focus_addr)" = "$NSAF"
		echo "asknos: foot='$NSAF' post='$(focus_addr)' probe='$NSAD' urgent-lines=$(grep -c 'urgent>>' "$STATE/asknos.cap" 2>/dev/null) client=[$(cat "$STATE/asknos.win.log" 2>/dev/null | tr '\n' '|')]" >&2
		kill "$NSP" 2>/dev/null
		wait "$NSLPID" 2>/dev/null
		wait_gone activatewin 2>/dev/null || true
	fi
	# the (b) probe may still be settling out; make sure the view is back on
	# workspace 1 before the foot-cleanup trailer
	dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 0.5
fi
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "focus: its foot is closed at the battery's end" \
	test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
