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

# ---- pixel-parity geometry (derived from hyprnotify/ui.hpp, ledger A-141) ----
# The island hangs off the monitor's right edge: EDGE 16 + CENTER_W 380, below
# offset_y 26. Card slots start at PAD 8 below the panel top; the footer
# (history circle, Clear all, DND circle) is FOOTER_H 52 tall with FOOTER_MT 16
# above it, so a single-card panel is 16 + cardH + 68 tall and the card bottom
# is panel_bottom - 76. Every click coordinate below is an inset from that
# fixed right-edge origin; the harness's MON_W keeps it monitor-relative.
N_EDGE=16
N_W=380
N_OFFSET=26
N_PAD=8
PANEL_X=$((MON_W - N_EDGE - N_W))
CARD1_Y=$((N_OFFSET + N_PAD))          # 34: the first card slot top
ROWX=$((PANEL_X + 150))                # card body: clears the avatar column (16..64)
ROWY=$((CARD1_Y + 37))                 # 71: inside a 74px collapsed card
CHIPX=$((PANEL_X + 343))               # the row-expand chevron hit (336..380 x)
KIDCHEV_X=$((PANEL_X + 343))           # the kid chevron hit, same column
KIDCHEV_Y=$((CARD1_Y + 82))            # 116: kid box starts at 34+12+48=94, its 44px hit spans 94..138
REPLY_BTN_X=$((PANEL_X + 116))         # the expanded kid's Reply button
REPLY_BTN_Y=186                        # expanded kid action row: 164..208
REPLY_FIELD_X=$((PANEL_X + 188))       # the armed field center (84..295 x)
REPLY_SEND_X=$((PANEL_X + 317))        # the send pill center
# the hold menu (long-press a card). Menu row boxes are full-width (350px);
# click x is the panel middle. The menu's own geometry is unchanged from v13;
# the panel pad 15->8 shifts every row 7px up and the footer block (16+52 vs
# 20+37) trims 3px off every panel height. Non-conversation target (card
# slot 1, top=34):
#   rows  (Default staged)    (Silent staged)     (Priority staged)
#   Priority    130             130                 139
#   Default     191 (62)        182                 200
#   Silent      252             243 (62)            252
#   Snooze      304             304                 304
#   options   356/408/460/512 when Snooze is open (every staging state)
#   buttons   364 closed, 572 open (every staging state)
#   panel h   446/428 closed, 654/636 open
# 1:1 conversation target: the header carries the chat title (+22 to every
# y above); buttons 386 closed / 594 open; panel h 468/450 / 676/658.
# Group conversation (bundle): no snooze section; buttons 334; panel h 416/406.
MENU_X=$((PANEL_X + 190))
MENU_PRIORITY_Y_DEFAULTSTAGED=130
MENU_DEFAULT_Y_DEFAULTSTAGED=191
MENU_SILENT_Y_DEFAULTSTAGED=252
MENU_DEFAULT_Y_SILENTSTAGED=182
MENU_SILENT_Y_SILENTSTAGED=243
MENU_PRIORITY_Y_PRIORITYSTAGED=139
MENU_DEFAULT_Y_PRIORITYSTAGED=200
MENU_SILENT_Y_PRIORITYSTAGED=252
MENU_SNOOZE_Y=304
MENU_OPT1_Y=356
MENU_OPT2_Y=408
MENU_OPT3_Y=460
MENU_OPT4_Y=512
MENU_DONE_Y=364
MENU_DONE_Y_OPEN=572
MENU_PRIORITY_Y_CONV=152
MENU_DEFAULT_Y_CONV_DEFAULTSTAGED=213
MENU_SILENT_Y_CONV_DEFAULTSTAGED=274
MENU_DEFAULT_Y_CONV_SILENTSTAGED=204
MENU_SILENT_Y_CONV_SILENTSTAGED=265
MENU_SNOOZE_Y_CONV=326
MENU_OPT2_Y_CONV=430
MENU_DONE_Y_CONV=386
MENU_DONE_Y_CONV_OPEN=594
# Bundle menu Done pill: 18px higher while Silent is staged (the Default row
# only wears its 2-line subtitle while Default is staged). Both verified.
MENU_DONE_Y_BUNDLE=326
MENU_DONE_Y_BUNDLE_SILENTSTAGED=298
MENU_DISMISS_X=$((PANEL_X + 74))
MENU_DONE_X=$((PANEL_X + 317))
# the manage-menu aliases the policy and pointer-only batteries click with. The
# manage menu IS the hold menu above: the row x is MENU_X, the plain rows are
# the Default-staged ys, and Done is the (now pill-centered) MENU_DONE_X. A
# right-click closes the panel from any row, so the "selected" alias just needs
# to land on the Default row of the default-staged layout.
ENTX=$MENU_X
MANAGE_PRIORITY_PLAIN_Y=$MENU_PRIORITY_Y_DEFAULTSTAGED
MANAGE_DEFAULT_PLAIN_Y=$MENU_DEFAULT_Y_DEFAULTSTAGED
MANAGE_SILENT_PLAIN_Y=$MENU_SILENT_Y_DEFAULTSTAGED
MANAGE_DEFAULT_SELECTED_Y=$MENU_DEFAULT_Y_DEFAULTSTAGED
MANAGE_DONE_X=$MENU_DONE_X
MANAGE_DONE_SINGLE_Y=$MENU_DONE_Y
MANAGE_DONE_CHAT_Y=$MENU_DONE_Y_CONV
MANAGE_DONE_GROUP_Y=$MENU_DONE_Y_BUNDLE
MANAGE_SNOOZE_NONCONV_Y=$MENU_SNOOZE_Y
MANAGE_SNOOZE_CHAT_Y=$MENU_SNOOZE_Y_CONV
# the OSD banner hangs off the same right edge as the shade; its icon cell is
# 16px in from the left, 16px below the y-34 band top.
POP_CARD_X=$PANEL_X
UNDO_X=$((PANEL_X + 330))             # the snooze row's right-aligned Undo
REPLY_FIELD_Y=190                     # armed field spans 168..212 inside the first kid
UNDO_Y=$((CARD1_Y + 37))              # 71: the 74px row's button center
# banner (HUN) geometry: the first banner is at y 26, 106px tall (plain) with
# its 44x66 chevron hit on the right edge
POP_CHEV_X=$((PANEL_X + 358))
POP_CHEV_Y=59

click() { # click <x> <y> <button-code>
	# A short settle: the input dispatches in <50ms; any pixel check after this
	# runs settle_frame, which re-captures until the spring lands.
	printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
		vp
	sleep 0.35
}
longpress() { # longpress <x> <y> <button-code>
	printf 'move %s %s\nsleep 80\npress %s\nsleep 650\nrelease %s\nsleep 120\n' "$1" "$2" "$3" "$3" |
		vp
	sleep 0.35
}
dragdown() { # dragdown <x> <y> <dy> — a downward drag past the 90px threshold
	local steps=5
	local per=$(( $3 / steps ))
	printf 'move %s %s\nsleep 60\npress 272\nsleep 40\n' "$1" "$2"
	for _ in $(seq 1 "$steps"); do printf 'rel 0 %s\nsleep 30\n' "$per"; done
	printf 'release 272\nsleep 120\n' | vp
	sleep 0.45
}
swipeh() { # swipeh <x> <y> <delta-px> — the horizontal history flick. A single
	# axis event with |delta| >= 90 crosses the swipe threshold; the virtual
	# pointer does not accumulate small chunks across separate events.
	printf 'move %s %s\nsleep 60\nscroll 1 %s\nsleep 250\n' "$1" "$2" "$3" | vp
	sleep 0.5
}
wheel() { # wheel <delta> — one vertical wheel nudge over the first card
	printf 'move %s %s\nsleep 60\nscroll 0 %s\nsleep 200\n' "$ROWX" "$ROWY" "$1" |
		vp
	sleep 0.3
}
# page_to <out> <want-panelh> <delta> — page the open shade's wheel in delta's
# direction until the panel settles at want. The virtual pointer's delta->step
# ratio is not 1:1 (the compositor synthesizes discrete steps), so we page in
# bounded repeats instead of one exact delta; s_skip clamps at the ends, so a
# few oversize nudges are harmless.
page_to() {
	local out=$1 want=$2 delta=$3 pb
	for _ in $(seq 1 8); do
		wheel "$delta"
		capture_nested "$out" || return 1
		pb=$(panel_bottom "$out")
		[[ $((pb - 26)) -eq "$want" ]] && return 0
	done
	return 1
}
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }
st() { hq hyprnotify state; }
bd() { hq hyprnotify badge; }
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
nfy() { # nfy <app> <summary> [body] — a plain card, no actions, no hints.
	# NOT notify-send (see the close-on-act note below): the card's whole
	# life stays hyprnotify's. Body defaults to one line (a 100px card);
	# pass "" for a title-only card (74px). ${3-body} (not :-) so an empty
	# body stays empty — a title-only card, not a silent fallback.
	local app=$1 sum=$2 body=${3-body}
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" "$body" 0 0 30000 >/dev/null 2>&1
}
nfyact() { # nfyact <app> <summary> <n-acts> <actions...> <n-hints> <hints...>
	# each hint is the busctl triple key type value, so n-hints entries are
	# 3*n-hints tokens on the wire
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

# panel_bottom <frame>: the shade's bottom rim row (the 1px white edge), or 0
# when no panel is on screen. The shade is the TOP island under the bar; the
# OSD band (hyprosd) sits a margin-gap BELOW it, so a bottom-up scan can land
# on the OSD's rim. Scan top-down instead: the panel's glass is non-black
# (~13,15,21) against a black (0,0,0) backdrop, so the shade is a contiguous
# run of non-black rows from its top. Stop at the first >=3-row black gap —
# that is the margin below the shade, before any OSD card.
panel_bottom() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.load()
x0 = int(sys.argv[2]) - 392 + 2   # panel left edge, inset past the corner
x1 = int(sys.argv[2]) - 12        # panel right edge, inset past the corner
# max, not min: a frost card row over the black backdrop is (4,6,9) — its
# min channel no longer clears the old threshold, while its max does. The
# shadow rows are neutral grey, where max == min, so the calibrated panel
# bottoms do not move.
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
	# Adaptive: re-capture until the open spring lands at want. 0.12s between
	# probes is far below the compositor's frame cadence, so a moving spring is
	# never mistaken for settled; a genuinely wrong value burns at most ~7s.
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
		# A /tmp copy survives the harness STATE cleanup for offline inspection.
		local pb; pb="$(panel_bottom "$2")"
		command cp -f "$2" "/tmp/capfail-$(basename "$2")" 2>/dev/null
		bad "$1 (want panel h $3, got $((pb - 26)))"
	fi
}

# ---- structured conversation sends ------------------------------------------
conversation_notify() { # app conversation-id title sender-id sender-name message-id body [kind] [sender-icon]
	local app=$1 conv=$2 title=$3 sender_id=$4 sender_name=$5 message_id=$6 body=$7 kind=${8:-one-to-one} sender_icon=${9:-}
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$title" "$body" 0 9 \
		desktop-entry s "$app" category s im.received \
		x-hyprnotify-conversation-id s "$conv" x-hyprnotify-conversation-title s "$title" \
		x-hyprnotify-conversation-kind s "$kind" x-hyprnotify-sender-id s "$sender_id" \
		x-hyprnotify-sender-name s "$sender_name" x-hyprnotify-sender-icon s "$sender_icon" \
		x-hyprnotify-message-id s "$message_id" 30000 >/dev/null 2>&1
}

conv_reply_notify() { # app conversation-id sender-id sender-name message-id text [kind]
	local app=$1 conv=$2 sid=$3 sname=$4 mid=$5 text=$6 kind=${7:-one-to-one}
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sname" "$text" 2 "inline-reply" "Reply" 8 \
		desktop-entry s "$app" category s im.received \
		x-hyprnotify-conversation-id s "$conv" x-hyprnotify-conversation-title s "$sname" \
		x-hyprnotify-conversation-kind s "$kind" x-hyprnotify-sender-id s "$sid" \
		x-hyprnotify-sender-name s "$sname" x-hyprnotify-message-id s "$mid" 60000 >/dev/null 2>&1
}

center_off() { [[ "$(st)" == center:1* ]] && { hq hyprnotify center >/dev/null; sleep 0.4; }; }
center_on() { [[ "$(st)" != center:1* ]] && { hq hyprnotify center >/dev/null; sleep 0.5; }; }
policy_lift() {
	local line tok app conv
	line=$(hq hyprnotify policy)
	[[ "$line" == "silenced:0 priority:0" ]] && return 0
	for tok in $(grep -o 's=[^ ]*' <<<"$line"); do
		app=${tok#s=}; app=${app%%+*} # a timed silence prints s=app+remaining
		nfy "$app" "policy lift"
		center_on
		longpress "$ROWX" "$ROWY" 272 # the menu opens Silent-staged: 431
		click "$MENU_X" "$MENU_DEFAULT_Y_SILENTSTAGED" 272
		click "$MENU_DONE_X" "$MENU_DONE_Y" 272
		hq hyprnotify clear >/dev/null 2>&1; sleep 0.4
	done
	for tok in $(grep -o 'p=[^ ]*' <<<"$line"); do
		app=${tok#p=}; app=${app%%/*}
		conv=${tok#*/} # the state line prints app<US>conv as app/conv
		conversation_notify "$app" "$conv" "$conv" sender Sender m-lift "hi"
		center_on
		longpress "$ROWX" "$ROWY" 272 # the menu opens Priority-staged: 453
		click "$MENU_X" 229 272 # the conv Default row under Priority staging
		click "$MENU_DONE_X" "$MENU_DONE_Y_CONV" 272
		hq hyprnotify clear >/dev/null 2>&1; sleep 0.4
	done
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

sec_notify() { # sec_notify <app> <summary> <group-key> — a declared-group card
	local app=$1 sum=$2 key=$3
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" body 0 3 \
		desktop-entry s "$app" x-hyprnotify-section s alerting \
		x-hyprnotify-group-key s "$key" 30000 >/dev/null 2>&1
}
