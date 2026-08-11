# ---- expiry, residency & the center ------------------------------------
# A normal banner runs its clock, then retreats to a resident center row while
# remaining in the model. Critical (urgency>=2) is the only sticky banner.
# Ephemerals vanish outright: transient and progress/OSD. The `state` line
# counts residents as `live` (raw model size, blind to the popup/shade
# split); the `badge` verb reads that split — "banners:N resident:N".
# Distinct -a apps here so popup coalescing (tested below) can't interfere.
st() { hq hyprnotify state; }
bd() { hq hyprnotify badge; }
chk "notif reset: clean state line" test "$(st)" = "center:0 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a crit -u critical \"urgent\" body')" # no -t: critical sticks
sleep 1
chk "critical: a sticky banner, nothing kept yet" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.exec_cmd('notify-send -a norm -t 600 normal body')" # explicit clock; retreats fast for the test
sleep 1.2
chk "residency: the expired normal banner retreated to a resident row" test "$(bd)" = "banners:1 resident:1"
chk "residency: the retreated card stays in the model, not lost" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a low -u low -t 600 lowcard body')" # low: retreats like a normal card
dsp "hl.dsp.exec_cmd('notify-send -a tran -e -t 600 trans body')"       # transient: VANISHES
sleep 1.2
chk "ephemerals: transient vanished, low parked resident" test "$(st)" = "center:0 live:3 dnd:0"
chk "ephemerals: only the critical banner is still up" test "$(bd)" = "banners:1 resident:2"
hq hyprnotify center >/dev/null; sleep 0.5
chk "center: opening absorbs the banner into the shade" test "$(bd)" = "banners:0 resident:3"
chk "center: opening dismisses nothing" test "$(st)" = "center:1 live:3 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.5
chk "center: closing neither re-pops nor drops a card" test "$(st)" = "center:0 live:3 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
chk "center: Clear all sweeps every visible card" test "$(st)" = "center:0 live:0 dnd:0"
# A plain (-1) normal card runs timeout_normal (5s) and retreats unattended.
dsp "hl.dsp.exec_cmd('notify-send \"default normal\" body')"
sleep 1
chk "default normal: pops as a banner" test "$(bd)" = "banners:1 resident:0"
sleep 5
chk "default normal: retreated to the shade after timeout_normal" test "$(bd)" = "banners:0 resident:1"
chk "default normal: the card is kept, not lost" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- popup coalescing: one live banner per app -------------------------
# spam control (coalesce_popups, default on): while an app shows a banner,
# further NON-CRITICAL same-app arrivals are born resident — one popup, the
# rest silent in the shade's fold. `badge` sees the split, `state` counts
# them all. Critical punches through; a different app gets its own banner.
dsp "hl.dsp.exec_cmd('notify-send -a chatty -t 30000 first body')"; sleep 1
chk "coalesce: the first same-app arrival pops a banner" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.exec_cmd('notify-send -a chatty second body')"
dsp "hl.dsp.exec_cmd('notify-send -a chatty third body')"; sleep 1
chk "coalesce: the same-app burst lands resident, one banner stands" test "$(bd)" = "banners:1 resident:2"
chk "coalesce: every card is kept and counted" test "$(st)" = "center:0 live:3 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a chatty -u critical urgent body')"; sleep 1
chk "coalesce: a critical from the same app punches through" test "$(bd)" = "banners:2 resident:2"
dsp "hl.dsp.exec_cmd('notify-send -a other elsewhere body')"; sleep 1
chk "coalesce: a different app gets its own banner" test "$(bd)" = "banners:3 resident:2"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- structured conversation identity (Android's MessagingStyle) --------
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
for m in one two three; do conversation_notify tg chat-alice "Shared title" alice Alice "alice-$m" "$m"; sleep 0.4; done
sleep 0.8
chk "conversation: 3 messages with one stable chat ID remain 1 card" test "$(st)" = "center:0 live:1 dnd:0"
conversation_notify tg chat-bob "Shared title" bob Bob bob-1 hello; sleep 1
chk "conversation: identical titles with different IDs remain distinct" test "$(st)" = "center:0 live:2 dnd:0"
conversation_notify tg chat-alice "Shared title" alice Alice alice-three edited; sleep 0.8
chk "conversation: a known message ID replaces in place" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain one\" body')"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain two\" body')"; sleep 1
chk "conversation: standard notifications never merge by visible text" test "$(st)" = "center:0 live:4 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# shade overflow: more rows than the monitor-tall panel holds must PAGE, not
# bleed off the bottom. 15 distinct-app cards -> 15 rows (one app each, so
# nothing bundles); explicit state selects each form and paging limits the view.
# Drawing it (the placement break plus the paging cue) must not crash and
# must keep every card.
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq hyprnotify center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- hardening: the shade's click model, absorb, DND, hostile hints -----
# A compact shade row reveals its hidden content before its body can act;
# once open, its BODY fires the card's primary and dismisses it. Exercise the
# exact merged-chat case through the real hit boxes via vptr. The panel hangs
# off the monitor's right edge
# (EDGE 16 + CENTER_W 380) below offset_y 34, so the first row's body is a
# fixed inset from the top-right corner. Hit boxes are final-position, so the
# open spring cannot move them out from under the click. Three structured
# Telegram-style messages merge into one row; the top row is system-expanded,
# and its body remains the action surface while the count pill alone expands.
for m in first second third; do
	conversation_notify Telegram chat-alice Alice alice Alice "hardening-$m" "$m"
	sleep 0.3
done
sleep 0.8
chk "shade: structured Telegram messages merge into one card" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.6
	click() { # click <x> <y> <button-code>
		printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
			vp
		sleep 0.8
	}
	longpress() { # longpress <x> <y> <button-code>
		printf 'move %s %s\nsleep 80\npress %s\nsleep 650\nrelease %s\nsleep 120\n' "$1" "$2" "$3" "$3" |
			vp
		sleep 0.8
	}
	NOTIFY_EDGE=16
	NOTIFY_W=380
	CENTER_BODY_PAD=10
	ROW_PAD=16
	ROW_ICON=40
	CENTER_PANEL_X=$((MON_W - NOTIFY_EDGE - NOTIFY_W))
	CENTER_ROW_X=$((CENTER_PANEL_X + CENTER_BODY_PAD))
	CENTER_ICON_X=$((CENTER_ROW_X + ROW_PAD + ROW_ICON / 2))
	CENTER_ICON_Y=$((34 + CENTER_BODY_PAD + ROW_PAD + ROW_ICON / 2))
	POP_CARD_X=$((MON_W - NOTIFY_EDGE - NOTIFY_W))
	POP_ICON_X=$((POP_CARD_X + ROW_PAD + ROW_ICON / 2))
	POP_ICON_Y=$((34 + ROW_PAD + ROW_ICON / 2))
	# Hold management starts at y=118. A selected mode is 62px high; the
	# other 24dp-icon rows are 44px, with 6px separation. Singleton panels
	# append Snooze after the policy rows; bundles do not.
	MANAGE_DEFAULT_SELECTED_Y=149
	MANAGE_DEFAULT_PLAIN_Y=140
	MANAGE_SILENT_PLAIN_Y=208
	MANAGE_PRIORITY_PLAIN_Y=140
	MANAGE_SNOOZE_NONCONV_Y=258
	MANAGE_SNOOZE_CHAT_Y=308
	MANAGE_DONE_SINGLE_Y=308
	MANAGE_DONE_CHAT_Y=358
	MANAGE_DONE_GROUP_Y=258
	MANAGE_DONE_X=$((MON_W - 68))
ENTX=$((MON_W - 200))
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }
ROWX=$((CENTER_ROW_X + 90)) # panel x + body pad + into the text column
ROWY=70                              # offset_y + body pad + into the first row
# The top row is system-expanded. Its body remains the primary surface; an
# actionless body click dismisses it without closing the shade.
click $ROWX $ROWY 272
chk "shade: open actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# Automatic groups include the section; app-declared groups override it.
section_notify() { # app summary section [declared-group]
	if [[ -n "${4:-}" ]]; then
		DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
			Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 3 desktop-entry s "$1" x-hyprnotify-section s "$3" x-hyprnotify-group-key s "$4" 30000 >/dev/null 2>&1
	else
		DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
			Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" x-hyprnotify-section s "$3" 30000 >/dev/null 2>&1
	fi
}
section_notify sections "alerting child" alerting
section_notify sections "silent child" silent
sleep 0.8; hq hyprnotify center >/dev/null; sleep 0.6
capture_nested "$STATE/grouping-sections.png"
click "$CENTER_ICON_X" "$CENTER_ICON_Y" 273
chk "grouping: different sections remain separate notifications" test "$(st)" = "center:1 live:1 dnd:0"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.6
section_notify declared "alerting child" alerting shared
section_notify declared "silent child" silent shared
sleep 0.8; hq hyprnotify center >/dev/null; sleep 0.6
capture_nested "$STATE/grouping-declared.png"
click "$CENTER_ICON_X" "$CENTER_ICON_Y" 273
chk "grouping: a declared group overrides section isolation" test "$(st)" = "center:1 live:0 dnd:0"
outside_click; sleep 0.5

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
# Firing a card's primary raises the sender over the very panel the click was
# made in, so the panel leaves with it — AOSP collapses the shade on a
# content-intent click, swaync ships the same as hide-on-action. `resident`
# is the fd.o way of saying the action does NOT take you away, and it holds
# the shade exactly as it holds the card. The actionless card above is the
# third case: nothing fired, so that click was only a dismissal.
#
# NOT notify-send, even though -A can send the action: notify-send WAITS for
# its action and EXITS the moment one fires, and libnotify closes the
# notification on the way out. The card then dies down the bus
# CloseNotification path rather than from the click, so "did the click keep
# the card?" would be measuring notify-send. busctl's call returns and
# leaves nothing behind — the card's whole life is hyprnotify's.
nfy() { # nfy <summary> <a{sv} hints...> — a card carrying a real `default`
	local sum="$1"; shift
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' gatechat 0 "" "$sum" body 2 default Open "$@" 30000 >/dev/null
}
nfy "open me" 0; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
nfy "stay me" 1 resident b true; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- hover holds a banner's clock ------------------------------------------
# A card must not expire out from under the pointer reading it. The pointer
# parks on the popup (top-right: EDGE 16 + width 380, below offset_y 34) for
# longer than the card's own timeout, then leaves — which RESTARTS the full
# clock rather than resuming the sliver that was left.
POPX=$((POP_CARD_X + NOTIFY_W / 2))
dsp "hl.dsp.exec_cmd('notify-send -t 1200 \"hold me\" body')"; sleep 0.4
printf 'move %s 64\nsleep 2400\n' "$POPX" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 0.4
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.4
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the bell's click path --------------------------------------------------
# The bell has no hover action. Its private bus interface exposes only the
# click-equivalent Toggle, which opens a real shade and absorbs the popped
# banner like any other explicit center open.
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
chk "bell: the removed hover method is absent" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; ! nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify | grep -q '\.Peek'"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"bell click\" body')"; sleep 1
chk "bell: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify Toggle >/dev/null 2>&1; sleep 0.5
chk "bell: click opens the shade" test "$(st)" = "center:1 live:1 dnd:0"
chk "bell: click absorbs the banner" test "$(bd)" = "banners:0 resident:1"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify Toggle >/dev/null 2>&1; sleep 0.4
chk "bell: second click closes the shade" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
chk "bell: reset after the click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- one top-left notification identity -----------------------------------
# Pixel uses one sender/avatar composition at the leading edge. Conversations
# may badge that avatar once with the application identity; every other card
# shows the application icon alone. Content/avatar hints must never create a
# second icon at the trailing edge or repeat inside bundle children.
IDENTITY_IMAGE="$STATE/identity-cyan.png"
AVATAR_IMAGE="$STATE/avatar-green.png"
CONTENT_IMAGE="$STATE/content-magenta.png"
magick -size 64x64 canvas:'#11dfe8' "$IDENTITY_IMAGE"
magick -size 64x64 canvas:'#20d45a' "$AVATAR_IMAGE"
magick -size 96x96 canvas:'#e935ff' "$CONTENT_IMAGE"
sample_rgb() {
	local file=$1 x=$2 y=$3 d=${4:-8}
	magick "$file" -crop "${d}x${d}+$((x - d / 2))+$((y - d / 2))" +repage -colorspace sRGB \
		-format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info: 2>/dev/null
}
is_cyan() { awk '{ exit !($1 < 0.25 && $2 > 0.65 && $3 > 0.65) }' <<<"$1"; }
is_green() { awk '{ exit !($1 < 0.30 && $2 > 0.60 && $3 < 0.50) }' <<<"$1"; }
is_not_magenta() { awk '{ exit !($1 < 0.65 || $2 > 0.40 || $3 < 0.65) }' <<<"$1"; }
cyan_pixel_count() {
	magick "$1" -crop "$2" +repage -colorspace sRGB -alpha off \
		-fx 'r<0.25&&g>0.65&&b>0.65?1:0' -format '%[fx:mean*w*h]' info: 2>/dev/null
}
magenta_pixel_count() {
	magick "$1" -crop "$2" +repage -colorspace sRGB -alpha off \
		-fx 'r>0.65&&g<0.40&&b>0.65?1:0' -format '%[fx:mean*w*h]' info: 2>/dev/null
}
green_pixel_count() {
	magick "$1" -crop "$2" +repage -colorspace sRGB -alpha off \
		-fx 'r<0.30&&g>0.60&&b<0.50?1:0' -format '%[fx:mean*w*h]' info: 2>/dev/null
}

# Notification identity textures are decoded asynchronously and a surface can
# be visible before its avatar/badge warm pass completes. Wait for two
# identical, semantically valid frames so identity assertions inspect a
# settled composition rather than an intermediate animation or warm frame.
capture_identity_composition() {
	local out=$1 lead_x=$2 lead_y=$3 badge_x=$4 badge_y=$5 crop=$6
	local previous= current= green= cyan=
	for _ in $(seq 1 30); do
		capture_nested "$out" || { sleep 0.1; continue; }
		green="$(sample_rgb "$out" "$lead_x" "$lead_y")"
		cyan="$(sample_rgb "$out" "$badge_x" "$badge_y" 6)"
		if is_green "$green" && is_cyan "$cyan"; then
			current="$(magick "$out" -crop "$crop" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1)"
			if [[ -n "$current" && "$current" == "$previous" ]]; then
				return 0
			fi
			previous=$current
		else
			previous=
		fi
		sleep 0.1
	done
	return 1
}

TRAIL_ICON_X=$((POP_CARD_X + 344))

ORDINARY_POPUP="$STATE/identity-ordinary-popup.png"
ORDINARY_CENTER="$STATE/identity-ordinary-center.png"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i identity-ordinary 0 "$IDENTITY_IMAGE" "Ordinary identity" "One leading app icon" 0 1 image-path s "$CONTENT_IMAGE" 30000 >/dev/null 2>&1
sleep 1
capture_nested "$ORDINARY_POPUP"
chk "identity: ordinary popup shows the application icon at top-left" is_cyan "$(sample_rgb "$ORDINARY_POPUP" "$POP_ICON_X" "$POP_ICON_Y")"
chk "identity: ordinary popup has no trailing content icon" is_not_magenta "$(sample_rgb "$ORDINARY_POPUP" "$TRAIL_ICON_X" "$POP_ICON_Y")"
ORDINARY_POP_MAGENTA="$(magenta_pixel_count "$ORDINARY_POPUP" "380x110+$POP_CARD_X+34")"
chk "identity: ordinary popup does not render content media elsewhere" awk "{ exit !(\$1 < 20) }" <<<"$ORDINARY_POP_MAGENTA"
hq hyprnotify center >/dev/null; sleep 0.6
capture_nested "$ORDINARY_CENTER"
ORDINARY_CENTER_CYAN="$(cyan_pixel_count "$ORDINARY_CENTER" "40x40+$((CENTER_ICON_X - 20))+$((CENTER_ICON_Y - 20))")"
chk "identity: ordinary center row shows one unbadged application icon" awk "{ exit !(\$1 > 900) }" <<<"$ORDINARY_CENTER_CYAN"
ORDINARY_CENTER_MAGENTA="$(magenta_pixel_count "$ORDINARY_CENTER" "360x110+$CENTER_ROW_X+44")"
chk "identity: ordinary center row does not render content media elsewhere" awk "{ exit !(\$1 < 20) }" <<<"$ORDINARY_CENTER_MAGENTA"
hq hyprnotify center >/dev/null; hq hyprnotify clear >/dev/null; sleep 0.8

# A singleton conversation count pill owns its complete 40dp hit target. It
# toggles presentation without leaking through to the default action under it.
SINGLE_EXPANDED="$STATE/single-count-expanded.png"
SINGLE_COLLAPSED="$STATE/single-count-collapsed.png"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i identity-count 0 "$IDENTITY_IMAGE" "Count target" "A long body that remains available after the compact presentation folds" \
	2 default Open 9 desktop-entry s identity-count category s im.received \
	x-hyprnotify-conversation-id s count-chat x-hyprnotify-conversation-title s "Count target" x-hyprnotify-conversation-kind s one-to-one \
	x-hyprnotify-sender-id s sender x-hyprnotify-sender-name s Sender x-hyprnotify-message-id s count-1 x-hyprnotify-unread-count u 7 30000 >/dev/null 2>&1
sleep 0.8; hq hyprnotify center >/dev/null; sleep 0.7
printf 'move %s %s\nsleep 120\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
capture_nested "$SINGLE_EXPANDED"
SINGLE_PILL_X=$((CENTER_ROW_X + 360 - ROW_PAD - 13))
click "$SINGLE_PILL_X" "$CENTER_ICON_Y" 272
chk "expansion: singleton count-pill hit does not invoke its body" test "$(st)" = "center:1 live:1 dnd:0"
printf 'move %s %s\nsleep 120\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
capture_nested "$SINGLE_COLLAPSED"
SINGLE_EXPANDED_HASH="$(magick "$SINGLE_EXPANDED" -crop "360x180+$CENTER_ROW_X+44" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1)"
SINGLE_COLLAPSED_HASH="$(magick "$SINGLE_COLLAPSED" -crop "360x180+$CENTER_ROW_X+44" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1)"
chk "expansion: singleton count pill changes the row presentation" test -n "$SINGLE_EXPANDED_HASH" -a "$SINGLE_EXPANDED_HASH" != "$SINGLE_COLLAPSED_HASH"
click "$((CENTER_ROW_X + 100))" "$CENTER_ICON_Y" 272
chk "expansion: the body remains a distinct primary-action target" test "$(st)" = "center:0 live:0 dnd:0"
sleep 0.5

# Generated identity is stable per conversation, media outranks that fallback,
# and group conversations use the two latest participants as a face pile.
generated_avatar_hash() {
	local out=$1
	capture_nested "$out" || return 1
	magick "$out" -crop "40x40+$((POP_ICON_X - 20))+$((POP_ICON_Y - 20))" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1
}
generated_conversation_notify() { # app app-icon conversation-id title message-id body
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "$2" "$4" "$6" 0 6 desktop-entry s "$1" category s im.received \
		x-hyprnotify-conversation-id s "$3" x-hyprnotify-conversation-title s "$4" \
		x-hyprnotify-conversation-kind s one-to-one x-hyprnotify-message-id s "$5" 30000 >/dev/null 2>&1
}
conversation_notify generated chat-ada Ada ada Ada generated-1 hello
sleep 0.8
GEN_A1="$(generated_avatar_hash "$STATE/generated-ada-1.png")"
hq hyprnotify clear >/dev/null; sleep 0.5
conversation_notify generated chat-ada Ada ada Ada generated-2 hello
sleep 0.8
GEN_A2="$(generated_avatar_hash "$STATE/generated-ada-2.png")"
hq hyprnotify clear >/dev/null; sleep 0.5
conversation_notify generated chat-bob Bob bob Bob generated-3 hello
sleep 0.8
GEN_B="$(generated_avatar_hash "$STATE/generated-bob.png")"
chk "identity: generated avatars are deterministic per conversation" test -n "$GEN_A1" -a "$GEN_A1" = "$GEN_A2"
chk "identity: different conversations receive different generated avatars" test -n "$GEN_B" -a "$GEN_A1" != "$GEN_B"
hq hyprnotify clear >/dev/null; sleep 0.5

generated_conversation_notify generated-title "$IDENTITY_IMAGE" shared "Alpha One" title-1 first
sleep 0.8
GEN_TITLE_A="$(generated_avatar_hash "$STATE/generated-title-alpha.png")"
generated_conversation_notify generated-title "$IDENTITY_IMAGE" shared "Beta Two" title-2 second
sleep 0.8
GEN_TITLE_B="$(generated_avatar_hash "$STATE/generated-title-beta.png")"
chk "identity: updating a generated conversation title rebuilds its initials" test -n "$GEN_TITLE_A" -a "$GEN_TITLE_A" != "$GEN_TITLE_B"
chk "identity: a generated-title update remains one conversation card" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.5

generated_conversation_notify generated-app-a "$IDENTITY_IMAGE" shared "Shared Name" app-a-1 first
sleep 0.8
GEN_APP_A="$(generated_avatar_hash "$STATE/generated-app-a.png")"
hq hyprnotify clear >/dev/null; sleep 0.5
generated_conversation_notify generated-app-b "$IDENTITY_IMAGE" shared "Shared Name" app-b-1 second
sleep 0.8
GEN_APP_B="$(generated_avatar_hash "$STATE/generated-app-b.png")"
chk "identity: identical conversation IDs from different apps stay visually distinct" test -n "$GEN_APP_A" -a "$GEN_APP_A" != "$GEN_APP_B"
hq hyprnotify clear >/dev/null; sleep 0.5

generated_conversation_notify rank-old "$IDENTITY_IMAGE" old-chat "Older Chat" rank-old-1 first
sleep 0.3
generated_conversation_notify rank-new "$AVATAR_IMAGE" new-chat "Newer Chat" rank-new-1 second
sleep 0.8
hq hyprnotify center >/dev/null; sleep 0.7
RANK_INITIAL="$STATE/conversation-rank-initial.png"
capture_nested "$RANK_INITIAL"
chk "ranking: the newer conversation initially leads its center tier" is_green "$(sample_rgb "$RANK_INITIAL" "$((CENTER_ICON_X + 12))" "$((CENTER_ICON_Y + 12))" 6)"
generated_conversation_notify rank-old "$IDENTITY_IMAGE" old-chat "Older Chat" rank-old-2 refreshed
sleep 0.8
RANK_REFRESHED="$STATE/conversation-rank-refreshed.png"
capture_nested "$RANK_REFRESHED"
chk "ranking: refreshing an older conversation moves it to the top" is_cyan "$(sample_rgb "$RANK_REFRESHED" "$((CENTER_ICON_X + 12))" "$((CENTER_ICON_Y + 12))" 6)"
chk "ranking: refreshing reorders without duplicating the conversation" test "$(st)" = "center:1 live:2 dnd:0"
hq hyprnotify center >/dev/null; hq hyprnotify clear >/dev/null; sleep 0.6

nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i media-chat 0 "$IDENTITY_IMAGE" Media "Content avatar wins" 0 9 desktop-entry s media-chat category s im.received \
	x-hyprnotify-conversation-id s media x-hyprnotify-conversation-title s Media x-hyprnotify-conversation-kind s one-to-one \
	x-hyprnotify-sender-id s sender x-hyprnotify-sender-name s Sender x-hyprnotify-message-id s media-1 image-path s "$AVATAR_IMAGE" 30000 >/dev/null 2>&1
sleep 1
MEDIA_FRAME="$STATE/identity-media-precedence.png"
capture_identity_composition "$MEDIA_FRAME" "$((POP_ICON_X - 10))" "$((POP_ICON_Y - 10))" "$((POP_ICON_X + 13))" "$((POP_ICON_Y + 13))" "380x110+$POP_CARD_X+34"
chk "identity: conversation media outranks a generated sender fallback" is_green "$(sample_rgb "$MEDIA_FRAME" "$((POP_ICON_X - 10))" "$((POP_ICON_Y - 10))")"
chk "identity: media-backed conversation keeps one application badge" is_cyan "$(sample_rgb "$MEDIA_FRAME" "$((POP_ICON_X + 13))" "$((POP_ICON_Y + 13))" 6)"
hq hyprnotify clear >/dev/null; sleep 0.5

conversation_notify group-chat room Team alice Alice face-a first group "$AVATAR_IMAGE"
conversation_notify group-chat room Team bob Bob face-b second group "$IDENTITY_IMAGE"
sleep 0.8
FACE_FRAME="$STATE/identity-face-pile.png"
capture_nested "$FACE_FRAME"
FACE_BACK="$(sample_rgb "$FACE_FRAME" "$((POP_ICON_X - 13))" "$((POP_ICON_Y - 13))" 4)"
FACE_FRONT="$(sample_rgb "$FACE_FRAME" "$POP_ICON_X" "$POP_ICON_Y" 4)"
chk "identity: group face pile contains two distinct participant surfaces" awk '
	{ dr=$1-a; dg=$2-b; db=$3-c; exit !(dr*dr+dg*dg+db*db > 0.01) }
' a="$(awk '{print $1}' <<<"$FACE_BACK")" b="$(awk '{print $2}' <<<"$FACE_BACK")" c="$(awk '{print $3}' <<<"$FACE_BACK")" <<<"$FACE_FRONT"
hq hyprnotify center >/dev/null; sleep 0.6
FACE_CENTER="$STATE/identity-face-pile-center.png"
capture_nested "$FACE_CENTER"
SENDER_GREEN="$(green_pixel_count "$FACE_CENTER" "40x160+$((CENTER_ROW_X + 8))+$((CENTER_ICON_Y + 18))")"
chk "identity: expanded group conversation paints sender-group avatars" awk "{ exit !(\$1 > 20) }" <<<"$SENDER_GREEN"
hq hyprnotify center >/dev/null; hq hyprnotify clear >/dev/null; sleep 0.6

CONV_POPUP="$STATE/identity-conversation-popup.png"
CONV_CENTER="$STATE/identity-conversation-center.png"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i identity-chat 0 "$IDENTITY_IMAGE" Alice "Avatar with one app badge" 0 2 category s im.received image-path s "$AVATAR_IMAGE" 30000 >/dev/null 2>&1
sleep 1
capture_identity_composition "$CONV_POPUP" "$((POP_ICON_X - 10))" "$((POP_ICON_Y - 10))" "$((POP_ICON_X + 13))" "$((POP_ICON_Y + 13))" "380x110+$POP_CARD_X+34"
chk "identity: conversation popup leads with the sender avatar" is_green "$(sample_rgb "$CONV_POPUP" "$((POP_ICON_X - 10))" "$((POP_ICON_Y - 10))")"
chk "identity: conversation popup carries one application badge" is_cyan "$(sample_rgb "$CONV_POPUP" "$((POP_ICON_X + 13))" "$((POP_ICON_Y + 13))" 6)"
CONV_POP_CYAN="$(cyan_pixel_count "$CONV_POPUP" "380x110+$POP_CARD_X+34")"
chk "identity: conversation popup has exactly one badge-sized app identity" awk "{ exit !(\$1 > 100 && \$1 < 800) }" <<<"$CONV_POP_CYAN"
hq hyprnotify center >/dev/null; sleep 0.6
capture_identity_composition "$CONV_CENTER" "$((CENTER_ICON_X - 9))" "$((CENTER_ICON_Y - 9))" "$((CENTER_ICON_X + 12))" "$((CENTER_ICON_Y + 12))" "360x110+$CENTER_ROW_X+44"
chk "identity: conversation center row leads with the sender avatar" is_green "$(sample_rgb "$CONV_CENTER" "$((CENTER_ICON_X - 9))" "$((CENTER_ICON_Y - 9))")"
chk "identity: conversation center row carries one application badge" is_cyan "$(sample_rgb "$CONV_CENTER" "$((CENTER_ICON_X + 12))" "$((CENTER_ICON_Y + 12))" 6)"
CONV_CENTER_CYAN="$(cyan_pixel_count "$CONV_CENTER" "360x110+$CENTER_ROW_X+44")"
chk "identity: conversation center row has exactly one badge-sized app identity" awk "{ exit !(\$1 > 100 && \$1 < 800) }" <<<"$CONV_CENTER_CYAN"
hq hyprnotify center >/dev/null; hq hyprnotify clear >/dev/null; sleep 0.8

FALLBACK_POPUP="$STATE/identity-chat-fallback-popup.png"
FALLBACK_CENTER="$STATE/identity-chat-fallback-center.png"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i identity-chat-fallback 0 "$IDENTITY_IMAGE" Bob "No sender avatar" 0 1 category s im.received 30000 >/dev/null 2>&1
sleep 1
capture_nested "$FALLBACK_POPUP"
chk "identity: avatar-less conversation popup expands the app icon" is_cyan "$(sample_rgb "$FALLBACK_POPUP" "$((POP_ICON_X - 10))" "$((POP_ICON_Y - 10))")"
hq hyprnotify center >/dev/null; sleep 0.6
capture_nested "$FALLBACK_CENTER"
chk "identity: avatar-less conversation center row uses one unbadged app icon" is_cyan "$(sample_rgb "$FALLBACK_CENTER" "$((CENTER_ICON_X - 9))" "$((CENTER_ICON_Y - 9))")"
hq hyprnotify center >/dev/null; hq hyprnotify clear >/dev/null; sleep 0.8

BUNDLE_CENTER="$STATE/identity-bundle-center.png"
for i in 1 2 3 4; do
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i identity-bundle 0 "$IDENTITY_IMAGE" "Bundle $i" "Text-only child $i" 0 1 image-path s "$CONTENT_IMAGE" 30000 >/dev/null 2>&1
done
sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
capture_nested "$BUNDLE_CENTER"
BUNDLE_CYAN="$(cyan_pixel_count "$BUNDLE_CENTER" "360x420+$CENTER_ROW_X+44")"
chk "identity: bundle header owns the only application icon" awk "{ exit !(\$1 > 150 && \$1 < 2200) }" <<<"$BUNDLE_CYAN"
GROUP_PILL_X=$((CENTER_ROW_X + 360 - 16 - 10))
GROUP_HEAD_Y=$((34 + 10 + 16 + 12))
click "$GROUP_PILL_X" "$GROUP_HEAD_Y" 272
chk "identity: the count-pill position changes expansion without dismissing" test "$(st)" = "center:1 live:4 dnd:0"
click "$GROUP_PILL_X" "$GROUP_HEAD_Y" 273
chk "identity: bundle right-click still dismisses the group" test "$(st)" = "center:1 live:0 dnd:0"
outside_click; sleep 0.5
