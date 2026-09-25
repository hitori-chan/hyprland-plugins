# focus — X11 EWMH pings must not take keyboard focus.
#
# Wine/Proton (the GOG installer, games) send _NET_ACTIVE_WINDOW on every
# internal SetForegroundWindow and map FlashWindowEx to
# _NET_WM_STATE_DEMANDS_ATTENTION. With misc's focus_on_activate on (the
# user's config, so a tray-icon Activate or a clicked notification raises the
# X11 app), the XWM took focus for every ping: the installer kept stealing the
# keyboard from the window the user worked in. The XWM now maps both pings to
# urgency only: the task chip turns red, focus stays. Explicit X11 activation
# happens plugin-side (the plugin that saw the user gesture focuses the app's
# window by PID — tray: hyprbar, actions: hyprnotify), and clicking the urgent
# window still focuses it.
#
# The probe (devtools/focustrap) maps an X11 toplevel; the gate parks focus on
# a Wayland window and starts the socket2 listener only AFTER that, so the
# capture window holds exactly the probe's delayed ping. Per mode the live
# focus must still name foot when the ping lands (the ground truth), the
# capture must hold an `urgent` event for the probe window (attention and
# activate), and a click on the urgent probe must focus it (the escape hatch).

FOCUSTRAP="$REPO/devtools/focustrap"
if [[ ! -x "$FOCUSTRAP" ]]; then
	make -C "$REPO/devtools" focustrap >/dev/null 2>&1 || true
fi
if [[ ! -x "$FOCUSTRAP" ]]; then
	bad "focus: $FOCUSTRAP is unavailable"
	return 0
fi

# The user's config has misc's focus_on_activate on; the stress config must
# match, or every check below is vacuous (with it off, activate() no-ops and
# the steal is invisible). Inject it into the stress config's misc block for
# the battery, restore it afterwards.
FOCUS_RULE=0
if ! grep -q "focus_on_activate" "$CFG" 2>/dev/null; then
	awk '{ print } /misc = \{/ && !inj { print "\t\tfocus_on_activate = 1,"; inj = 1 }' \
		"$CFG" >"$CFG.focus" && mv "$CFG.focus" "$CFG" && FOCUS_RULE=1
fi
if [[ "$FOCUS_RULE" == 1 ]]; then
	hq reload >/dev/null 2>&1
	sleep 2
fi
restore_focus_cfg() {
	if [[ "$FOCUS_RULE" == 1 ]]; then
		grep -v "focus_on_activate = 1," "$CFG" >"$CFG.restore" 2>/dev/null \
			&& mv "$CFG.restore" "$CFG" && hq reload >/dev/null 2>&1
	fi
}

# Nested XWayland display: the compositor's Xwayland child advertises it on
# its own cmdline.
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
	restore_focus_cfg
	return 0
fi

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

probe_addr() { # the probe's hyprctl address without the 0x (socket2's format)
	clients | python3 -c "
import json,sys
a = next((c['address'] for c in json.load(sys.stdin) if c['class']=='focustrap'), '')
print(a[2:] if a.startswith('0x') else a)"
}
focus_addr() { # the active window's address, socket2 format (no 0x)
	hq activewindow 2>/dev/null | awk '/^Window/ { print $2; exit }'
}
click_center() { # click_center <class> — primary click at the window's center
	clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class']=='$1'), None)
print(int(c['at'][0]+c['size'][0]/2), int(c['at'][1]+c['size'][1]/2)) if c else (0,0)" \
	| { read -r cx cy; printf "move %s %s\nsleep 50\npress 272\nsleep 50\nrelease 272\nsleep 50\n" "$cx" "$cy"; } | vp
}

dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"
sleep 2

run_ping_mode() { # run_ping_mode <mode> — map, park focus, ping, assert
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
	# The ping lands 5s after the map (≈3s into the capture); leave room.
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

# This battery's foot would otherwise survive into the tray battery: the
# remembered spot (the windows battery's hostile-tsv clamp) puts it at the
# bottom-right corner, right under the menu column, and its bright columns
# poison every panel-extent capture. Close it and verify, as the windows
# battery does for its own seeds.
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 0.5
chk "focus: its foot is closed before the geometry batteries" \
	test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0

# Leave the stress config as the rest of the gate found it.
restore_focus_cfg
sleep 0.5
