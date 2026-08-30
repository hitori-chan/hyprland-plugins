#!/usr/bin/env bash
# Shared helpers for the notification batteries and for isolated probes.
# Pure definitions only — no side effects, no battery code — so any module
# (or a standalone probe) can source this at any point after `retarget`:
# the geometry constants derive from MON_W, which retarget sets.
#
# Batteries source this file instead of relying on execution order, and a
# probe needs nothing more than:
#   source .../stress/probe-env.sh && retarget
#   source .../stress/notify-lib.sh

# ---- v6 geometry (derived from hyprnotify/ui.hpp) ---------------------------
# Two surfaces hang off the monitor's top-right corner below the bar:
#  * the ISLAND — the banner column: N_EDGE 10 off the right edge, width N_W
#    (config width, default 348), first card at N_OFFSET 34 below the top,
#    stacked down with N_MARGIN 6 gaps.
#  * the SHADE — the center: a CENTER_W 360 panel in the same corner. Rows
#    start 10 (BODY_PADT) below the panel top; a collapsed row is ROW_H 59
#    (9 + max(icon 40, text) + 10) with STACK_GAP 3 between rows. The footer
#    (DND, rules, Clear all) is BAR_H 50 tall (4 + 34 + 12).
N_EDGE=10
N_W=348
CENTER_W=360
N_OFFSET=34
N_MARGIN=6
ROW_H=59
ISL_X=$((MON_W - N_EDGE - N_W))
PANEL_X=$((MON_W - N_EDGE - CENTER_W))
ROWX=$((PANEL_X + 10 + 80))             # row-1 body: panel x + body pad + into the text column
ROWY=64                                 # offset + body pad + into the first row (spans 44..103)
CHVX=$((MON_W - 10 - 10 - 12 - 12))     # row-1 chevron center: right edge - body pad - ROW_PADX - half CHEV
OVX=$((MON_W - 20 - 12 - 32 - 12))      # row-1 manage-⋮ center, riding the same row
ENTX=$((MON_W - 200))                   # anywhere inside a manage entry's width
ent() { echo $((N_OFFSET + 10 + 9 + 28 + $1 * 28 + 14)); }  # manage entry i center y
POPX=$((MON_W - N_EDGE - N_W / 2))      # the first banner's center x (the hover-hold park)
DND_X=$((PANEL_X + 10 + 17))            # the footer DND ⊖ (34×34 at panel x + 10)

# ---- input -------------------------------------------------------------------
click() { # click <x> <y> <button-code>
	# A short settle: the input dispatches in <50ms; any pixel check after this
	# runs settle_frame, which re-captures until the spring lands.
	printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
		vp
	sleep 0.35
}
tap() { # tap <key> — one virtual-keyboard tap: a named key (down, up, esc,
	# space, delete, enter, tab) or a US keycode (35=h, 23=i, 25=p, 31=s,
	# 22=u, 50=m)
	printf 'tap %s\nsleep 250\n' "$1" | vk
	sleep 0.6
}
swipe() { # swipe <x> <y> <delta> — the horizontal row wheel. Three ±25
	# increments accumulate past the ±60 threshold: right (positive)
	# dismisses the row, left (negative) opens its manage panel.
	printf 'move %s %s\nsleep 60\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 200\n' "$1" "$2" "$3" "$3" "$3" |
		vp
	sleep 0.9
}
wheel() { # wheel <delta> — one vertical wheel nudge over the first row
	printf 'move %s %s\nsleep 60\nscroll 0 %s\nsleep 200\n' "$ROWX" "$ROWY" "$1" |
		vp
	sleep 0.3
}
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }
peek() { # peek <true|false> — the bell-hover path on the plugin's own bus
	# interface: opens the shade without pinning it (a leaving pointer
	# inside the grace window cancels it), true opens, false closes.
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications \
		org.hitori.hyprnotify Peek b "$1" >/dev/null 2>&1
	# the open/close is deferred to the event loop and the close sits behind
	# the 400ms grace: settle before any state read, or the read races it
	sleep 0.6
	sleep 0.5
}

# ---- model strings -------------------------------------------------------------
st() { hq hyprnotify state; }    # center:X live:Y dnd:Z — the raw model size
bd() { hq hyprnotify badge; }    # banners:N resident:M — the popup/shade split
sz() { hq hyprnotify snoozed; }  # the snoozed count (still in the model)
pol() { hq hyprnotify policy; }  # silenced:N s=app[+secs] priority:M p=app/sender
polsil() { pol | sed 's/ priority:.*//'; }
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }

# ---- sends -----------------------------------------------------------------------
# None of these is notify-send: notify-send WAITS for its actions and EXITS
# the moment one fires, closing the notification on the way out, so a card's
# whole life would be measured against libnotify rather than hyprnotify.
# busctl's call returns and leaves nothing behind.
nfy() { # nfy <app> <summary> [body] — a plain card, no actions, no hints.
	local app=$1 sum=$2 body=${3-body}
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" "$body" 0 0 30000 >/dev/null 2>&1
}
nfyact() { # nfyact <app> <summary> <n-acts> <actions...> <n-hints> <hints...>
	local app=$1 sum=$2; shift 2
	local nacts=$1 acts=(); shift
	local i
	for i in $(seq 1 "$nacts"); do acts+=("$1"); shift; done
	local nhints=$1 hints=(); shift
	for i in $(seq 1 $((nhints * 3))); do hints+=("$1"); shift; done
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" body \
		"$nacts" ${acts[@]+"${acts[@]}"} "$nhints" ${hints[@]+"${hints[@]}"} \
		30000 >/dev/null 2>&1
}
psend() { # psend <app> <sender> [category] — an explicit desktop-entry, so
	# the app key never depends on how a sender happens to fill the hint
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" category s "$3" 30000 >/dev/null 2>&1
}
pcrit() { # pcrit <app> <sender> — a critical arrival under the app key
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" urgency y 2 30000 >/dev/null 2>&1
}
ptran() { # ptran <app> <sender> — a transient chat. Transient keeps its
	# banner through an absorb, which is what makes two cards tellable
	# apart in the badge
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 3 desktop-entry s "$1" category s im.received transient b true 30000 >/dev/null 2>&1
}
conv_notify() { # conv_notify <app> <sender> <message> — a v6 conversation
	# card: the summary IS the sender (the merge key) and the message is
	# the body
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 0 1 desktop-entry s "$1" category s im.received 30000 >/dev/null 2>&1
}
conv_reply_notify() { # conv_reply_notify <app> <sender> <message> — a
	# conversation card that also offers inline-reply
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 2 inline-reply Reply 1 category s im.received 60000 >/dev/null 2>&1
}
nfyid() { # nfyid <app> <summary> — nfy, printing the id the bus returns
	local app=$1 sum=$2
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" body 0 0 30000 2>/dev/null |
		awk 'NR==1{print $2}'
}
closeid() { # closeid <id> — CloseNotification by the returned id
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		CloseNotification u "$1" >/dev/null 2>&1
}

# ---- center / policy -------------------------------------------------------------
center_off() { [[ "$(st)" == center:1* ]] && { hq hyprnotify center >/dev/null; sleep 0.4; }; }
center_on() { [[ "$(st)" != center:1* ]] && { hq hyprnotify center >/dev/null; sleep 0.5; }; }
# policy_lift — leave no rule standing between batteries. v6 toggles a rule
# by key on the SELECTED row (m = mute app, p = mark sender), and selection
# starts nowhere, so: send the app a fresh card (newest = the top row), open
# the shade, ↓ selects it, m/p lifts. A muted app still lands resident, so
# the card is always there to be selected.
policy_lift() {
	local line tok app sender
	line=$(pol)
	[[ "$line" == "silenced:0 priority:0" ]] && return 0
	for tok in $(grep -o 's=[^ ]*' <<<"$line"); do
		app=${tok#s=}; app=${app%%+*} # a timed silence prints s=app+remaining
		psend "$app" "policy lift" ""
		sleep 1
		center_on
		tap down
		tap 50 # m
		tap esc # the lift leaves the shade open; the next battery starts closed
		hq hyprnotify clear >/dev/null 2>&1; sleep 0.5
	done
	for tok in $(grep -o 'p=[^ ]*' <<<"$line"); do
		app=${tok#p=}; app=${app%%/*}
		sender=${tok#p=*/} # the state line prints app/sender
		# the same card shape the mark was SET on (the p verb only answers to
		# a conversation card carrying that app + sender)
		psend "$app" "$sender" im.received
		sleep 1
		center_on
		tap down
		tap 25 # p
		tap esc # as above — leave the shade the way the batteries found it
		hq hyprnotify clear >/dev/null 2>&1; sleep 0.5
	done
}

# ---- pixels ------------------------------------------------------------------------
# panel_bottom <frame>: the shade's bottom rim row (the 1px edge), or 0 when
# no panel is on screen. The shade is the top-right island; a bottom-up scan
# could land on something below it, so scan top-down instead: the panel's
# fill is non-black against a black (0,0,0) backdrop, so it is a contiguous
# run of non-black rows from its top. Stop at the first >=3-row black gap —
# that is the margin below the shade.
panel_bottom() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.load()
x0 = int(sys.argv[2]) - 392 + 2   # panel left edge, inset past the corner
x1 = int(sys.argv[2]) - 12        # panel right edge, inset past the corner
def filled(y):
    for x in range(x0, x1, 3):
        if max(px[x, y]) > 5:
            return True
    return False
H = im.size[1]
top = next((y for y in range(26, H) if filled(y)), None)
if top is None:
    print(0)
    raise SystemExit
bot = top
for y in range(top, H):
    if filled(y):
        bot = y
    elif all(not filled(yy) for yy in range(y, min(y + 3, H))):
        break
print(bot)
PY
}
# settle_frame <out> <want-panelh> — capture until the open spring lands. The
# final layout is what the hit boxes reference, so a mid-spring frame is
# indistinguishable from a wrong height: retry rather than assert on it.
settle_frame() {
	local out=$1 want=$2 pb
	for _ in $(seq 1 12); do
		capture_nested "$out" || return 1
		pb="$(panel_bottom "$out")"
		[[ $((pb - 26)) -eq "$want" ]] && return 0
		sleep 0.12
	done
	return 1
}
# expect_panel <label> <frame> <want-panelh>
expect_panel() {
	if settle_frame "$2" "$3"; then
		ok "$1"
	else
		# The last settle_frame capture is the settled frame; report what it
		# actually landed at so a wrong value is a one-line fix, not a re-run.
		local pb; pb="$(panel_bottom "$2")"
		command cp -f "$2" "/tmp/capfail-$(basename "$2")" 2>/dev/null
		bad "$1 (want panel h $3, got $((pb - 26)))"
	fi
}
