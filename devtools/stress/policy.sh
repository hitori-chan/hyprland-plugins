#!/usr/bin/env bash
# The policy battery: marked conversations (persisted), the manage panel,
# the horizontal row swipe, ranking, absorb idempotence, DND queueing, and
# hostile hints. Helpers live in notify-lib.sh; this file is battery code
# only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"
POLFILE="$STATE/hyprnotify/policy.tsv"

# ---- the persistence fixture ----------------------------------------------
# The mark is a REAL one, written before this instance existed: it is the
# end-to-end proof that a mark outlives the session that made it. Seeded here
# (store format, app + US + sender) because the batteries before this one
# lift every standing mark.
mkdir -p "$STATE/hyprnotify"
printf 'p\tkeepapp\x1fKeeper\n' > "$POLFILE"
kill_nested
launch_nested || { echo "policy fixture relaunch FAILED"; exit 1; }
retarget || { echo "policy fixture retarget FAILED"; exit 1; }
chk "policy: the mark from disk survived the relaunch" test "$(pol)" = "priority:1 p=keepapp/Keeper"

# ---- per-app policy: marked conversations -----------------------------------
# DND on/off was the whole vocabulary, and "this person first" is neither of
# its two answers. The mark toggles by key on the SELECTED shade row
# (p = mark sender) and persists, so every assertion is made twice: once on
# the live behaviour, once on the store.
psend keepapp Keeper im.received; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 25 # p
# if the disk mark had NOT loaded, this would be a SET, not a lift — the
# line would read priority:1 instead of priority:0
chk "policy: p lifted the persisted mark" test "$(pol)" = "priority:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
# marking a conversation: the key is app + sender, because one chat app
# carries many people
psend chatapp Alice im.received; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 25 # p
chk "policy: p marked the sender" test "$(pol)" = "priority:1 p=chatapp/Alice"
chk "policy: the mark reached the disk" grep -qxF "$(printf 'p\tchatapp\x1fAlice')" "$POLFILE"
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
chk "policy: the mark outlives the card it was set on" test "$(pol)" = "priority:1 p=chatapp/Alice"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "policy: reset after the policy battery" test "$(st)" = "center:0 live:0 dnd:0"
# the summary-keyed mark is lifted before the id block: the id block's
# assertions read the WHOLE policy line, and the lift rides the same by-key
# path the block below exercises
policy_lift
chk "policy: the summary mark lifted" test "$(pol)" = "priority:0"

# the STABLE-id contract (Telegram's): the mark keys on the conversation-id,
# not the title. The store half is the same as above; the RANK half is what
# the summary-only re-derive got wrong — the visible card's flag must land at
# the mark, else the newer unmarked chat sorts on top of the marked one until
# the card's next replace. The newcomer is transient so the two stay tellable
# in the badge, exactly as above.
pcid cidapp "Carol" cid-carol; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 25 # p
chk "policy: p marked the conversation-id" test "$(pol)" = "priority:1 p=cidapp/cid-carol"
chk "policy: the id mark reached the disk" grep -qxF "$(printf 'p\tcidapp\x1fcid-carol')" "$POLFILE"
tap esc; sleep 0.5
ptran cidapp2 "Dan" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "policy: the id mark and a newer transient one" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "policy: the TOP row was the id-marked chat, not the newer one" test "$(bd)" = "banners:1 resident:0"
chk "policy: the id mark outlives the card it was set on" test "$(pol)" = "priority:1 p=cidapp/cid-carol"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
policy_lift # leave no id mark standing for the later blocks
chk "policy: the id mark lifted" test "$(pol)" = "priority:0"
chk "policy: reset after the id-mark battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the manage panel -------------------------------------------------------
# Everything the row can do beyond fire/dismiss is a named entry in the panel
# the ⋮ opens (or the back-swipe, tested below). Driven through the REAL hit
# boxes: the ⋮ rides where the hover strip used to (panel right edge -
# ROW_PADX - RTRIM - OVER_D/2), and the entries stack from below the panel's
# own header at 28px each. A non-chat card's panel is a single entry:
# Dismiss.
psend mgr "manage me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
click $OVX 64 272
chk "manage: the ⋮ opened the panel — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $ENTX "$(ent 0)" 272 # "Dismiss"
chk "manage: the dismiss entry dismissed the card, panel and all" test "$(st)" = "center:1 live:0 dnd:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "manage: reset after the manage battery" test "$(st)" = "center:0 live:0 dnd:0"

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
# one per app: the newest re-pops, the same-app sibling stays parked
chk "absorb: the close re-popped the newest, the sibling parked (one per app)" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.8

# The lost-notification bug: the corner dead-strip next to a conversation is
# the common stray outside click — the close must re-pop what the open
# absorbed, not leave the stack invisible in the closed panel
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"repop me\" body')"; sleep 1
chk "repop: a live banner before the open" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify center >/dev/null; sleep 0.7
chk "repop: the open absorbed it" test "$(bd)" = "banners:0 resident:1"
outside_click; sleep 0.8
chk "repop: the outside-click close popped it back" test "$(bd)" = "banners:1 resident:0"
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

# ---- the module leaves no mark standing -------------------------------------
# The policy battery's mark outlives its card (that is the point of it), so
# lift it here — the next battery must start from a clean store.
policy_lift
chk "policy: no mark left behind" test "$(pol)" = "priority:0"
chk "policy: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "policy: no banners or residents" test "$(bd)" = "banners:0 resident:0"
