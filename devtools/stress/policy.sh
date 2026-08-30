#!/usr/bin/env bash
# The policy battery: silenced apps and marked senders (both persisted), the
# manage panel with its timed mutes, the undo window behind a snooze, the
# horizontal row swipe, snooze and its wake, ranking, absorb idempotence,
# DND queueing, and hostile hints. Helpers live in notify-lib.sh; this file
# is battery code only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"
POLFILE="$STATE/hyprnotify/policy.tsv"

# ---- the persistence fixture ----------------------------------------------
# The rule the hostile-file battery left behind is a REAL one, written before
# this instance existed: it is the end-to-end proof that a rule outlives the
# session that made it. Seeded here (hostile-file format) because the
# batteries before this one lift every standing rule.
mkdir -p "$STATE/hyprnotify"
printf 's\tkeepme\n' > "$POLFILE"
kill_nested
launch_nested || { echo "policy fixture relaunch FAILED"; exit 1; }
retarget || { echo "policy fixture retarget FAILED"; exit 1; }
chk "policy: the rule from disk survived the relaunch" test "$(pol)" = "silenced:1 s=keepme priority:0"

# ---- per-app policy: silenced apps, marked conversations --------------------
# DND on/off was the whole vocabulary, and "never this app" and "this person
# first" are neither of its two answers. Both rules toggle by key on the
# SELECTED shade row (m = mute app, p = mark sender) and both persist, so
# every assertion is made twice: once on the live behaviour, once on the
# store.
psend keepme "quiet please" ""; sleep 1.2
chk "policy: the persisted rule silences a fresh arrival" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50 # m
chk "policy: m lifted the persisted rule" test "$(pol)" = "silenced:0 priority:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise one" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50 # m
chk "policy: m silenced the app" test "$(pol)" = "silenced:1 s=spammer priority:0"
# a silence carries its expiry as a third field; 0 is the one that never lifts
chk "policy: the rule reached the disk" grep -qxF "$(printf 's\tspammer\t0')" "$POLFILE"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise two" ""; sleep 1.2
chk "policy: a silenced app's card lands with no banner" test "$(bd)" = "banners:0 resident:1"
pcrit spammer "alarm"; sleep 1.2
chk "policy: critical still punches through a silenced app" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise three" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50
chk "policy: m again lifts the silence" test "$(pol)" = "silenced:0 priority:0"
chk "policy: the store emptied with it" test ! -s "$POLFILE"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise four" ""; sleep 1.2
chk "policy: the app banners again" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; sleep 0.6
# marking a conversation: the key is app + sender, because one chat app
# carries many people
psend chatapp Alice im.received; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 25 # p
chk "policy: p marked the sender, not the app" test "$(pol)" = "silenced:0 priority:1 p=chatapp/Alice"
tap esc; sleep 0.5
# and the mark outranks a NEWER chat from someone else. The newcomer is
# transient so it keeps its banner through the absorb — that is what tells
# the two apart in the badge after the top row is deleted.
ptran chatapp2 Bob; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "policy: the marked chat and a newer transient one" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "policy: the TOP row was the marked chat, not the newer one" test "$(bd)" = "banners:1 resident:0"
chk "policy: the mark outlives the card it was set on" test "$(pol)" = "silenced:0 priority:1 p=chatapp/Alice"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "policy: reset after the policy battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the manage panel, timed mutes, the rule count --------------------------
# Everything the row can do beyond fire/dismiss is a named entry in the panel
# the ⋮ opens (or the back-swipe, tested below). Driven through the REAL hit
# boxes: the ⋮ rides where the hover strip used to (panel right edge -
# ROW_PADX - RTRIM - OVER_D/2), and the entries stack from below the panel's
# own header at 28px each.
# The battery above deliberately leaves a MARK standing (it outlives its
# card), so these read the silence half alone rather than the whole line.
psend mgr "manage me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
click $OVX 64 272
chk "manage: the ⋮ opened the panel — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "manage: opening it set no rule" test "$(polsil)" = "silenced:0"
click $ENTX "$(ent 2)" 272 # "Mute mgr for 1 hour"
# A timed rule prints its seconds remaining, and that is a clock read —
# assert the SHAPE (timed at all, and near the hour asked for), never the
# exact value.
# +3600 is possible: a read under a second after the mute rounds the
# remaining hour back up — the shape is "timed, near the hour asked for".
chk "manage: a timed mute landed WITH an expiry" bash -c "[[ '$(polsil)' == silenced:1\ s=mgr+3[56][0-9][0-9] ]]"
chk "manage: the expiry reached the disk" grep -qE "^s	mgr	[0-9]{10}$" "$POLFILE"
psend mgr "still muted" ""; sleep 1.2
chk "manage: a timed rule silences an arrival exactly as a permanent one does" test "$(bd)" = "banners:0 resident:2"
# and the way back out: silenced, the panel offers Unmute where the three
# durations were, so a rule is never one you cannot find the end of
click $OVX 64 272
click $ENTX "$(ent 2)" 272 # "Unmute mgr"
chk "manage: the panel lifted the rule" test "$(polsil)" = "silenced:0"
chk "manage: and no silence is left on disk" bash -c "! grep -q '^s	' '$POLFILE'"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "manage: reset after the manage battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the undo window behind a snooze ----------------------------------------
# The card now holds its slot as an undo row for a few seconds, so `snoozed`
# goes up and comes back DOWN without the card ever having left the model.
sz() { hq hyprnotify snoozed; }
# 12s: comfortably past the 6s undo window so the lapse is testable without
# racing the wake (8s left under a second of margin, and the key injection
# alone spends most of that), yet short enough that the card wakes and can be
# swept before the next battery. A snoozed card is unreachable by design —
# Clear all does not cancel a reminder — so letting it wake is the ONLY way
# to leave this battery clean.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 12 } } })' >/dev/null; sleep 0.4
psend undoer "take it back" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 31 # s
chk "undo: s snoozed the card" test "$(sz)" = 1
tap 22 # u
chk "undo: u took the snooze back" test "$(sz)" = 0
chk "undo: and the card never left the model" test "$(st)" = "center:1 live:1 dnd:0"
tap 31 # s again — and this time let the window lapse
chk "undo: snoozed again" test "$(sz)" = 1
sleep 7
chk "undo: the window lapsed and the snooze stands" test "$(sz)" = 1
tap 22 # u past the window is not an undo — there is no row to have clicked
chk "undo: u past the window does nothing" test "$(sz)" = 1
sleep 6
chk "undo: and it still woke on its own clock" test "$(sz)" = 0
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "undo: reset after the undo battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- swipe: the horizontal wheel on a row -----------------------------------
# An ADDITION on top of the pointer path — a mouse without a horizontal wheel
# must lose no verb — so this asserts the gesture works, not that it is the
# only way. Away dismisses; back opens the manage panel.
psend swiper "flick me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "swipe: a card in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
swipe $ENTX 64 -25
chk "swipe: back opened the manage panel, it did not dismiss" test "$(st)" = "center:1 live:1 dnd:0"
# esc peels the panel, not the shade — the panel's own ⋮ sits further right
# than a row's, so this is also the assertion that the peel order is right
tap esc
chk "swipe: esc peeled the panel and left the shade up" test "$(st)" = "center:1 live:1 dnd:0"
swipe $ENTX 64 25
chk "swipe: away dismissed the row" test "$(st)" = "center:1 live:0 dnd:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "swipe: reset after the swipe battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- snooze -----------------------------------------------------------------
# "Remind me": the card leaves the shade outright (Android's snooze — no
# section, nothing to scroll past) and comes back ALERTING. It is still in
# the model the whole time, which is exactly what tells a snooze apart from
# a dismissal: `state` counts it, the badge does not. snooze_seconds is
# turned down to 2 so the wake is testable at all.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 2 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a later -t 60000 \"read this eventually\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "snooze: the card is up and in the shade" test "$(bd)" = "banners:0 resident:1"
tap down
tap 31 # s
chk "snooze: it left the shade" test "$(bd)" = "banners:0 resident:0"
chk "snooze: but it is still in the model, not dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "snooze: and it counts as snoozed" test "$(sz)" = 1
hq hyprnotify clear >/dev/null; sleep 0.5
chk "snooze: Clear all does not cancel a reminder" test "$(sz)" = 1
sleep 2.5
chk "snooze: it came back" test "$(sz)" = 0
chk "snooze: and it came back ALERTING, not merely parked" test "$(bd)" = "banners:1 resident:0"
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze: reset after the snooze battery" test "$(st)" = "center:0 live:0 dnd:0"

# A snoozed chat whose sender keeps talking must STAY away. The conversation
# merge deliberately aims a chat's new messages at the one card that holds
# it, and a replace re-alerts by design — so without an explicit exception
# the snooze ended at the next thing the sender said, which is the one case
# it exists for. The card must take the message and no banner with it.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 5 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a tgz -c im.received -t 60000 Zoe first')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 31 # s
chk "snooze/merge: the chat card is away" test "$(sz)" = 1
dsp "hl.dsp.exec_cmd('notify-send -a tgz -c im.received -t 60000 Zoe second')"; sleep 1.2
chk "snooze/merge: a new message does not wake it" test "$(sz)" = 1
chk "snooze/merge: and it took no banner" test "$(bd)" = "banners:0 resident:0"
chk "snooze/merge: still one card, not two" test "$(st)" = "center:1 live:1 dnd:0"
sleep 6
chk "snooze/merge: the wake still arrives on its own clock" test "$(bd)" = "banners:1 resident:0"
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze/merge: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# Ranking: a critical sorts to the top however late the others arrived. The
# two cards are made TELLABLE APART in the badge — a transient one opts out
# of residency, so opening the shade absorbs the critical and leaves it a
# banner — and the keyboard deletes whatever the top row is. Both wrong
# answers (older-first, or an injector that did nothing) read differently.
dsp "hl.dsp.exec_cmd('notify-send -e -t 60000 -a chat \"an ordinary card\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -a alarm -u critical \"disk failing\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "ranking: an absorbed critical beside an unabsorbed transient" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "ranking: the TOP row was the critical, not the card that came first" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# absorb is idempotent: toggling the center never loses or dupes a card
dsp "hl.dsp.exec_cmd('notify-send \"keep one\" body')"
dsp "hl.dsp.exec_cmd('notify-send \"keep two\" body')"; sleep 1
for i in 1 2 3; do hq hyprnotify center >/dev/null; sleep 0.35; done # on, off, on
chk "absorb: three toggles leave the two cards intact" bash -c "hyprctl -i $SIG hyprnotify state | grep -qE '^center:1 live:2 '"
hq hyprnotify center >/dev/null; sleep 0.35 # off
hq hyprnotify clear >/dev/null; sleep 0.8

# DND queues arrivals silently; the resume keeps them AND applies the same
# one-per-app cap (the resume banner assignment is coalesce-aware — this
# guards it alongside the absorb path).
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.5
chk "DND arms" hq_matches 'dnd:1' hyprnotify state
dsp "hl.dsp.exec_cmd('notify-send -a q one body')"
dsp "hl.dsp.exec_cmd('notify-send -a q two body')"; sleep 0.8
chk "DND: two same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.6
chk "DND resume: one popped, the sibling resumed resident (one per app)" test "$(bd)" = "banners:1 resident:1"
chk "DND resume: dnd off, both cards kept" hq_matches '^center:0 live:2 dnd:0$' hyprnotify state
hq hyprnotify clear >/dev/null; sleep 0.8

# hostile hints: a wrong-typed category must not crash the parse (sdbus::Error
# thrown + caught), the card still lands
dsp "hl.dsp.exec_cmd('notify-send -h int:category:5 \"badcat\" body')"
dsp "hl.dsp.exec_cmd('notify-send -h string:category:im.received \"convo\" body')"; sleep 1
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the module leaves no rule standing -------------------------------------
# The policy battery's mark outlives its card (that is the point of it), so
# lift it here — the next battery must start from a clean store.
policy_lift
chk "policy: no rule left behind" test "$(pol)" = "silenced:0 priority:0"
chk "policy: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "policy: no snooze in flight" test "$(sz)" = 0
chk "policy: no banners or residents" test "$(bd)" = "banners:0 resident:0"
