# ---- per-app policy: silenced apps, marked conversations --------------------
# DND on/off was the whole vocabulary, and "never this app" and "this person
# first" are neither of its two answers. Both rules persist, so every
# assertion is made twice: once on the live behaviour, once on the store.
# The Notify calls go over the bus with an explicit desktop-entry — the app
# key must not depend on how notify-send happens to fill the hint.
POLFILE="$STATE/hyprnotify/policy.tsv"
pol()   { hq hyprnotify policy; }
psend() {
	if [[ "$3" == im || "$3" == im.* ]]; then
		nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
			Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 5 desktop-entry s "$1" category s "$3" \
			x-hyprnotify-conversation-id s "conv-$2" x-hyprnotify-conversation-title s "$2" x-hyprnotify-conversation-kind s one-to-one 30000 >/dev/null 2>&1
	else
		nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
			Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" category s "$3" 30000 >/dev/null 2>&1
	fi
}
pcrit() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" urgency y 2 30000 >/dev/null 2>&1; }
ptran() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 6 desktop-entry s "$1" category s im.received transient b true \
		x-hyprnotify-conversation-id s "conv-$2" x-hyprnotify-conversation-title s "$2" x-hyprnotify-conversation-kind s one-to-one 30000 >/dev/null 2>&1; }
# the rule the hostile-file battery left behind is a REAL one, written before
# this instance existed: it is the end-to-end proof that a rule outlives the
# session that made it
chk "policy: the rule from disk survived the relaunch" test "$(pol)" = "silenced:1 s=keepme priority:0"
psend keepme "quiet please" ""; sleep 1.2
chk "policy: the persisted rule silences a fresh arrival" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_DEFAULT_PLAIN_Y" 272
chk "policy: selection is staged until Done" test "$(pol)" = "silenced:1 s=keepme priority:0"
click "$MANAGE_DONE_X" "$MANAGE_DONE_SINGLE_Y" 272
chk "policy: m lifted the persisted rule" test "$(pol)" = "silenced:0 priority:0"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise one" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_SILENT_PLAIN_Y" 272
chk "policy: Silent is staged before Done" test "$(pol)" = "silenced:0 priority:0"
click "$MANAGE_DONE_X" "$MANAGE_DONE_SINGLE_Y" 272
chk "policy: m silenced the app" test "$(pol)" = "silenced:1 s=spammer priority:0"
# a silence carries its expiry as a third field; 0 is the one that never lifts
chk "policy: the rule reached the disk" grep -qxF "$(printf 's\tspammer\t0')" "$POLFILE"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise two" ""; sleep 1.2
chk "policy: a silenced app's card lands with no banner" test "$(bd)" = "banners:0 resident:1"
pcrit spammer "alarm"; sleep 1.2
chk "policy: critical still punches through a silenced app" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise three" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_DEFAULT_PLAIN_Y" 272
click "$MANAGE_DONE_X" "$MANAGE_DONE_SINGLE_Y" 272
chk "policy: m again lifts the silence" test "$(pol)" = "silenced:0 priority:0"
chk "policy: the store emptied with it" test ! -s "$POLFILE"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise four" ""; sleep 1.2
chk "policy: the app banners again" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; sleep 0.6
# marking a conversation: the key is app + stable conversation ID, because one
# chat application carries many conversations
psend chatapp Alice im.received; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_PRIORITY_PLAIN_Y" 272
chk "policy: Priority is staged before Done" test "$(pol)" = "silenced:0 priority:0"
click "$MANAGE_DONE_X" "$MANAGE_DONE_CHAT_Y" 272
chk "policy: p marked the stable conversation, not the title" test "$(pol)" = "silenced:0 priority:1 p=chatapp/conv-Alice"
outside_click; sleep 0.5
# and the mark outranks a NEWER chat from someone else. The newcomer is
# transient so it keeps its banner through the absorb — that is what tells
# the two apart in the badge after the top row is deleted.
ptran chatapp2 Bob; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "policy: the marked chat and a newer transient one" test "$(bd)" = "banners:1 resident:1"
click "$ROWX" "$ROWY" 273
chk "policy: the TOP row was the marked chat, not the newer one" test "$(bd)" = "banners:1 resident:0"
chk "policy: the mark outlives the card it was set on" test "$(pol)" = "silenced:0 priority:1 p=chatapp/conv-Alice"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.8
chk "policy: reset after the policy battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the staged Pixel hold modes and the rule count -------------------------
# Android exposes notification management through a press-and-hold gesture.
# Long-press turns the target into a full-width Priority/Default/Silent panel.
# Mode clicks only stage a choice; Done commits it, while right-click discards
# the staged choice. Snooze is a separate labelled singleton action here.
# the battery above deliberately leaves a MARK standing (it outlives its card),
# so these read the silence half alone rather than the whole line
polsil() { hq hyprnotify policy | sed 's/ priority:.*//'; }
psend mgr "manage me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
longpress $ROWX $ROWY 272
chk "manage: long-press opened the panel — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "manage: opening it set no rule" test "$(polsil)" = "silenced:0"
click $ENTX "$MANAGE_SILENT_PLAIN_Y" 272
chk "manage: selecting Silent did not commit early" test "$(polsil)" = "silenced:0"
click "$MANAGE_DONE_X" "$MANAGE_DONE_SINGLE_Y" 272
chk "manage: Done committed Silent" test "$(polsil)" = "silenced:1 s=mgr"
chk "manage: the permanent rule reached disk" grep -qxF "$(printf 's\tmgr\t0')" "$POLFILE"
psend mgr "still muted" ""; sleep 1.2
chk "manage: Silent suppresses a fresh banner" test "$(bd)" = "banners:0 resident:2"
longpress $ROWX $ROWY 272
click $ENTX "$MANAGE_DEFAULT_PLAIN_Y" 272
click $ENTX "$MANAGE_DEFAULT_SELECTED_Y" 273
chk "manage: right-click discarded the staged Default" test "$(polsil)" = "silenced:1 s=mgr"
longpress $ROWX $ROWY 272
click $ENTX "$MANAGE_DEFAULT_PLAIN_Y" 272
# The second same-app card forms a Pixel group at two, so this uses the bundle
# management surface.
click "$MANAGE_DONE_X" "$MANAGE_DONE_GROUP_Y" 272
chk "manage: Done committed Default and lifted the rule" test "$(polsil)" = "silenced:0"
chk "manage: and no silence is left on disk" bash -c "! grep -q '^s	' '$POLFILE'"
outside_click
hq hyprnotify clear >/dev/null; sleep 0.8
for i in 1 2 3 4; do psend bundle "bundle $i" ""; sleep 0.2; done; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "manage/group: four same-app cards form one digest" test "$(st)" = "center:1 live:4 dnd:0"
longpress $ROWX $ROWY 272
chk "manage/group: long-press opened the bundle panel" test "$(st)" = "center:1 live:4 dnd:0"
click $ENTX "$MANAGE_SILENT_PLAIN_Y" 272
click "$MANAGE_DONE_X" "$MANAGE_DONE_GROUP_Y" 272
chk "manage/group: bundle Silent committed at app scope" test "$(polsil)" = "silenced:1 s=bundle"
longpress $ROWX $ROWY 272
click $ENTX "$MANAGE_DEFAULT_PLAIN_Y" 272
click "$MANAGE_DONE_X" "$MANAGE_DONE_GROUP_Y" 272
chk "manage/group: bundle panel lifted the rule" test "$(polsil)" = "silenced:0"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.8
chk "manage: reset after the manage battery" test "$(st)" = "center:0 live:0 dnd:0"

# Clear all owns the whole track left after DND. With a normal management
# panel open, its footer center is deterministic; this x is inside the full
# track but well left of the removed centered 96px chip.
psend cleartrack "full footer target" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
CLEAR_TRACK_X=$((MON_W - 300))
CLEAR_TRACK_Y=369
click "$CLEAR_TRACK_X" "$CLEAR_TRACK_Y" 272
chk "Clear all: the expanded footer track owns its left side" test "$(st)" = "center:1 live:0 dnd:0"
outside_click; sleep 0.5

# ---- the undo window behind a snooze ----------------------------------------
# The card holds its slot as an undo row for a few seconds, so `snoozed` rises
# and returns to zero without the card leaving the model.
sz() { hq hyprnotify snoozed; }
# 12s: comfortably past the 6s undo window so the lapse is testable without
# racing the wake (8s left under a second of margin, and the key injection
# alone spends most of that), yet short enough that the card wakes and can be
# swept before the next battery. A snoozed card is unreachable by design —
# Clear all does not cancel a reminder — so letting it wake is the ONLY way to
# leave this battery clean.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 12 } } })' >/dev/null; sleep 0.4
psend undoer "take it back" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_SNOOZE_NONCONV_Y" 272
chk "undo: the hold-menu Snooze action snoozed the card" test "$(sz)" = 1
UNDOX=$((MON_W - 48))
click "$UNDOX" "$ROWY" 272
chk "undo: the pointer Undo control restored the card" test "$(sz)" = 0
chk "undo: and the card never left the model" test "$(st)" = "center:1 live:1 dnd:0"
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_SNOOZE_NONCONV_Y" 272
chk "undo: snoozed again" test "$(sz)" = 1
sleep 7
chk "undo: the window lapsed and the snooze stands" test "$(sz)" = 1
click "$UNDOX" "$ROWY" 272 # past the window there is no undo hitbox
chk "undo: a late Undo click does nothing" test "$(sz)" = 1
sleep 6
chk "undo: and it still woke on its own clock" test "$(sz)" = 0
outside_click; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "undo: reset after the undo battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- swipe: the horizontal wheel on a row -----------------------------------
# An ADDITION on top of the pointer path — a mouse without a horizontal wheel
# must lose no verb — so this asserts the gesture works, not that it is the
# only way. Away dismisses; back opens the manage panel.
swipe() { printf 'move %s %s\nsleep 60\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 200\n' "$1" "$2" "$3" "$3" "$3" | vp; sleep 0.9; }
psend swiper "flick me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "swipe: a card in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
swipe "$ROWX" "$ROWY" -25
chk "swipe: back opened the manage panel, it did not dismiss" test "$(st)" = "center:1 live:1 dnd:0"
# A right-click closes the gesture-opened panel while leaving the shade up.
click "$ENTX" "$MANAGE_DEFAULT_SELECTED_Y" 273
chk "swipe: right-click peeled the panel and left the shade up" test "$(st)" = "center:1 live:1 dnd:0"
swipe "$ROWX" "$ROWY" 25
chk "swipe: away dismissed the row" test "$(st)" = "center:1 live:0 dnd:0"
outside_click; hq hyprnotify clear >/dev/null; sleep 0.8
chk "swipe: reset after the swipe battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- snooze -----------------------------------------------------------------
# "Remind me": the card leaves the shade outright (Android's snooze — no
# section, nothing to scroll past) and comes back ALERTING. It is still in the
# model the whole time, which is exactly what tells a snooze apart from a
# dismissal: `state` counts it, the badge does not. snooze_seconds is turned
# down to 4 so the hold-menu gesture remains testable without racing it.
# `hyprctl keyword` is refused by the Lua parser ("use eval"), so the knob is
# turned the way the live config would turn it
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 4 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a later -t 60000 \"read this eventually\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "snooze: the card is up and in the shade" test "$(bd)" = "banners:0 resident:1"
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_SNOOZE_NONCONV_Y" 272
chk "snooze: it left the shade" test "$(bd)" = "banners:0 resident:0"
chk "snooze: but it is still in the model, not dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "snooze: and it counts as snoozed" test "$(sz)" = 1
hq hyprnotify clear >/dev/null; sleep 0.5
chk "snooze: Clear all does not cancel a reminder" test "$(sz)" = 1
sleep 4.5
chk "snooze: it came back" test "$(sz)" = 0
chk "snooze: and it came back ALERTING, not merely parked" test "$(bd)" = "banners:1 resident:0"
outside_click; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze: reset after the snooze battery" test "$(st)" = "center:0 live:0 dnd:0"

# A snoozed chat whose sender keeps talking must STAY away. The conversation
# merge deliberately aims a chat's new messages at the one card that holds it,
# and a replace re-alerts by design — so without an explicit exception the
# snooze ended at the next thing the sender said, which is the one case it
# exists for. The card must take the message and no banner with it.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 5 } } })' >/dev/null; sleep 0.4
conversation_notify tgz chat-zoe Zoe zoe Zoe snooze-first first; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
longpress "$ROWX" "$ROWY" 272
click "$ENTX" "$MANAGE_SNOOZE_CHAT_Y" 272
chk "snooze/merge: the chat card is away" test "$(sz)" = 1
conversation_notify tgz chat-zoe Zoe zoe Zoe snooze-second second; sleep 1.2
chk "snooze/merge: a new message does not wake it" test "$(sz)" = 1
chk "snooze/merge: and it took no banner" test "$(bd)" = "banners:0 resident:0"
chk "snooze/merge: still one card, not two" test "$(st)" = "center:1 live:1 dnd:0"
sleep 6
chk "snooze/merge: the wake still arrives on its own clock" test "$(bd)" = "banners:1 resident:0"
outside_click; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze/merge: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# Ranking: a critical sorts to the top however late the others arrived. The
# two cards are made TELLABLE APART in the badge — a transient one opts out of
# residency, so opening the shade absorbs the critical and leaves it a banner.
# A pointer right-click deletes the top row. Both wrong answers (older-first or
# an input path that did nothing) read differently.
dsp "hl.dsp.exec_cmd('notify-send -e -t 60000 -a chat \"an ordinary card\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -a alarm -u critical \"disk failing\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "ranking: an absorbed critical beside an unabsorbed transient" test "$(bd)" = "banners:1 resident:1"
click "$ROWX" "$ROWY" 273
chk "ranking: the TOP row was the critical, not the card that came first" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# absorb is idempotent: toggling the center never loses or dupes a card
dsp "hl.dsp.exec_cmd('notify-send \"keep one\" body')"
dsp "hl.dsp.exec_cmd('notify-send \"keep two\" body')"; sleep 1
for i in 1 2 3; do hq hyprnotify center >/dev/null; sleep 0.35; done # on, off, on
chk "absorb: three toggles leave the two cards intact" hq_matches '^center:1 live:2 ' hyprnotify state
hq hyprnotify center >/dev/null; sleep 0.35 # off
hq hyprnotify clear >/dev/null; sleep 0.8

# DND queues arrivals silently; the resume keeps them AND applies the same
# one-per-app cap (the resume banner assignment is now coalesce-aware — this
# guards it alongside the buildDisplay/absorb changes)
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.5
chk "DND arms" hq_matches 'dnd:1' hyprnotify state
dsp "hl.dsp.exec_cmd('notify-send -a q one body')"
dsp "hl.dsp.exec_cmd('notify-send -a q two body')"; sleep 0.8
chk "DND: two same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.6
chk "DND resume: one popped, the sibling resumed resident (one per app)" test "$(bd)" = "banners:1 resident:1"
chk "DND resume: dnd off, both cards kept" hq_matches '^center:0 live:2 dnd:0$' hyprnotify state
hq hyprnotify clear >/dev/null; sleep 0.8

# hostile hints on the new field: a wrong-typed category must not crash the
# parse (sdbus::Error thrown + caught), the card still lands
dsp "hl.dsp.exec_cmd('notify-send -h int:category:5 \"badcat\" body')"
dsp "hl.dsp.exec_cmd('notify-send -h string:category:im.received \"convo\" body')"; sleep 1
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# Retained notification state is deliberately bounded. The transport has
# already copied this payload, but the model must parse only its first body
# cap, retain four thumbnails and twelve action pairs, and stay responsive.
FLOOD="$({ for i in $(seq 1 32); do printf '<img src="/no-such-image/%s" alt="image">&' "$i"; done; head -c 20000 /dev/zero | tr '\0' '&'; })"
FLOOD_ACTIONS=()
for i in $(seq 1 24); do
	FLOOD_ACTIONS+=("action-$i" "Action $i")
done
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i flood 0 "" flood "$FLOOD" "${#FLOOD_ACTIONS[@]}" "${FLOOD_ACTIONS[@]}" 0 30000 >/dev/null 2>&1
sleep 1
chk "admission: hostile markup/actions/images leave one responsive card" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
