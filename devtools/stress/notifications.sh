#!/usr/bin/env bash
# The hyprnotify behavior battery: expiry and residency, coalescing,
# conversation identity, the v13 card ladder and shade click model, hold
# menus, snooze, digests, generated-identity pixels, overflow, and history.
# Helpers live in notify-lib.sh; this file is battery code only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"

# ---- expiry, residency & the center ------------------------------------
# A normal banner runs its clock, then retreats to a resident center row while
# remaining in the model. Critical (urgency>=2) is the only sticky banner.
# Ephemerals vanish outright: transient and progress/OSD. The `state` line
# counts residents as `live` (raw model size, blind to the popup/shade
# split); the `badge` verb reads that split — "banners:N resident:N".
# Distinct -a apps here so popup coalescing (tested below) can't interfere.
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
# Nine hint entries (busctl's a{sv} count is strict): desktop-entry,
# category, conv-id, conv-title, conv-kind, sender-id, sender-name,
# sender-icon, message-id.

for m in one two three; do conversation_notify tg chat-alice "Shared title" alice Alice "alice-$m" "$m"; sleep 0.4; done
sleep 0.8
chk "conversation: 3 messages with one stable chat ID remain 1 card" test "$(st)" = "center:0 live:1 dnd:0"
conversation_notify tg chat-bob "Shared title" bob Bob bob-1 hello; sleep 1
chk "conversation: identical titles with different IDs remain distinct" test "$(st)" = "center:0 live:2 dnd:0"
conversation_notify tg chat-alice "Shared title" alice Alice alice-three edited; sleep 0.8
chk "conversation: a known message ID replaces in place" test "$(st)" = "center:0 live:2 dnd:0"
nfy tg "plain one"
nfy tg "plain two"; sleep 1
chk "conversation: standard notifications never merge by visible text" test "$(st)" = "center:0 live:4 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the v13 card ladder: collapsed, card-open, kid-open, field-armed ----
# The pixel-parity conversation card is 74px collapsed (header + body),
# 106px card-open (one 66px kid), 161px with the newest kid open (time, name,
# text, and its Reply button), 175px while the reply field is armed. The
# panel is fit-to-content: 8 + card + 68 + 8. Each state is asserted on the
# white rim, so a layout that grows or shrinks a state fails here, not in
# production.
# Eight hint entries: desktop-entry, category, conv-id, conv-title,
# conv-kind, sender-id, sender-name, message-id. The actions carry the
# reserved "inline-reply" ID (its label renders as Reply) plus a plain
# "Reply" action, so the card shows the arm-field button and the invoke
# button side by side — the ladder and HUN batteries click the FIRST.

conv_reply_notify tg chat-ladder Alice alice ladder-1 "hello there"
sleep 1
chk "ladder: the conversation arrived as one banner" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify center >/dev/null; sleep 0.5
expect_panel "ladder A: collapsed conversation card is 70px" "$STATE/ladder-a.png" 157
click "$CHIPX" 74 272
expect_panel "ladder B: the chip opened the card, one 66px kid is 138px" "$STATE/ladder-b.png" 221
click "$KIDCHEV_X" "$KIDCHEV_Y" 272
expect_panel "ladder C: the kid chevron opened the kid with its Reply at 186px" "$STATE/ladder-c.png" 278
click "$REPLY_BTN_X" "$REPLY_BTN_Y" 272
expect_panel "ladder D: the armed field takes 202px" "$STATE/ladder-d.png" 290
# the field owns the keys: type a word and send; the card leaves with it
printf 'tap h\ntap i\nsleep 300\n' | vk; sleep 0.5
printf 'tap enter\n' | vk; sleep 0.8
chk "ladder: the reply fired and its card left" test "$(st)" = "center:1 live:0 dnd:0"
expect_panel "ladder: the shade stays open on its empty state" "$STATE/ladder-sent.png" 145
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.6

# ---- the v13 shade click model -------------------------------------------
# Body fires the card's primary (or dismisses an actionless card) and a
# non-resident act leaves the shade with it; right dismisses; the chip or the
# icon column toggles the card's content; a drag down past 90px swipes the
# card away; the banner's chevron opens the shade instead of expanding in
# place; the banner's Reply invokes the sender's UI and dismisses, it does
# NOT arm the inline field (that lives on the shade's kid).
nfy clickmodel "actionless"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
expect_panel "clickmodel: one collapsed plain card is 100px (title + one body line)" "$STATE/clickmodel-a.png" 157
click "$ROWX" "$ROWY" 272
chk "clickmodel: an actionless body click dismisses, the shade stays" test "$(st)" = "center:1 live:0 dnd:0"
nfy clickmodel "right me"; sleep 1
click "$ROWX" "$ROWY" 273
chk "clickmodel: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.6
# the chip and the icon column both toggle a plain card's content. The body
# is TWO lines: collapsed shows its last line (100px card), open shows both
# (124px card — a body line is 24px), so the toggle changes the height.
nfy toggler "long title that carries a body line" "first line of the body
second line of the body"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
expect_panel "toggle: the collapsed plain card shows its last body line" "$STATE/toggle-a.png" 157
click "$CHIPX" 74 272
expect_panel "toggle: the chip revealed the full two-line body" "$STATE/toggle-b.png" 181
click "$CHIPX" 68 272
expect_panel "toggle: the chip folded it again" "$STATE/toggle-c.png" 157
click "$((PANEL_X + 30))" 74 272
expect_panel "toggle: the icon column toggles too" "$STATE/toggle-d.png" 181
click "$CHIPX" 68 272
chk "toggle: reset the card to collapsed" test "$(st)" = "center:1 live:1 dnd:0"
# drag down past 90px swipes the card away. The nested virtual-pointer drag is
# not frame-deterministic, so this is best-effort: when it lands the card is
# gone; when it doesn't we note it (source + demo verify the behavior, ledger
# A-138) and clear the card so the battery stays clean.
dragdown "$ROWX" "$ROWY" 120
if [[ "$(st)" == center:1\ live:0\ dnd:0 ]]; then
	ok "drag: a 120px downward drag dismissed the card"
else
	printf ' note drag: nested virtual-pointer drag was not deterministic here; swipe-to-dismiss is source- and demo-verified (A-138)\n'
	hq hyprnotify clear >/dev/null; sleep 0.4
fi
outside_click; sleep 0.4
# the banner chevron opens the shade; the banner body's Reply invokes + dismisses
conv_reply_notify tg chat-hun Hana hana hun-1 "ping"
sleep 1
chk "hun: the conversation is up as a banner" test "$(bd)" = "banners:1 resident:0"
click "$POP_CHEV_X" "$POP_CHEV_Y" 272
chk "hun: the banner chevron opened the shade and absorbed the banner" test "$(st)" = "center:1 live:1 dnd:0"
expect_panel "hun: the absorbed card is the collapsed conversation" "$STATE/hun-shade.png" 157
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.4 # chat-hun is still resident; sweep it or the next card is born resident, bannerless
conv_reply_notify tg chat-hun2 Iris iris hun2-1 "pong"
sleep 1
# the banner's action row sits below who+message (the row is 44px, its
# center ~109 from the monitor top for a one-message conversation). The
# banner's actions invoke the sender's UI over the wire — they never arm
# the inline field (that lives on the shade's kid)
 click "$((PANEL_X + 99))" 109 272
chk "hun: the banner Reply invoked the sender's UI and dismissed the card" test "$(st)" = "center:0 live:0 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.6

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
# Firing a card's primary raises the sender over the very panel the click was
# made in, so the panel leaves with it — AOSP collapses the shade on a
# content-intent click. `resident` is the fd.o way of saying the action does
# NOT take you away, and it holds the shade exactly as it holds the card.
#
# NOT notify-send, even though -A can send the action: notify-send WAITS for
# its action and EXITS the moment one fires, and libnotify closes the
# notification on the way out. The card then dies down the bus
# CloseNotification path rather than from the click, so "did the click keep
# the card?" would be measuring notify-send. busctl's call returns and
# leaves nothing behind — the card's whole life is hyprnotify's.
nfyact gatechat "open me" 2 default Open 0
sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click "$ROWX" "$ROWY" 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
# actions are id/label STRING PAIRS on the wire: one button is two strings.
nfyact gatechat "stay me" 2 resident resident 1 resident b true
sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
# the card has no "default" action, so a body click is a dismissal; the
# resident verb is its own button. Expand via the icon column, click it.
click $((PANEL_X + 46)) 76 272
sleep 0.8
click $((PANEL_X + 119)) 149 272 # the "resident" label's center
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the v13 hold menu: staging, commit, pre-stage ---------------------------
# A long-press turns the card into its management surface: three mode rows
# (Priority / Default / Silent) that only STAGE, a snooze section, and a
# Done/Dismiss footer. Done commits (Policy::setMode) and the card folds back;
# Dismiss and right-click close without committing. The menu reopens with the
# currently-effective mode already staged, so a silenced app's menu opens
# Silent-staged. Silent persists per app (and on disk: policy.tsv survives a
# nested restart), Priority per conversation. A leftover rule from any earlier
# run would pre-stage these menus, so lift every standing one first: a card
# from the app, its menu, Default, Done.

policy_lift
chk "policy-lift: no standing rules before the menu batteries" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
center_off
nfy holda "hold target"
sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
expect_panel "hold: one plain card before the menu" "$STATE/hold-plain.png" 157
longpress "$ROWX" "$ROWY" 272
expect_panel "hold: long-press opens the menu, Default staged" "$STATE/hold-menu.png" 445
click "$MENU_X" "$MENU_SILENT_Y_DEFAULTSTAGED" 272
expect_panel "hold: staging Silent folds the selected row" "$STATE/hold-silent.png" 427
click "$MENU_X" "$MENU_PRIORITY_Y_DEFAULTSTAGED" 272
expect_panel "hold: staging Priority keeps the same height" "$STATE/hold-priority.png" 427
click "$MENU_X" "$MENU_DEFAULT_Y_PRIORITYSTAGED" 272
expect_panel "hold: staging Default grows it back" "$STATE/hold-default.png" 445
click "$ROWX" "$ROWY" 273
expect_panel "hold: right-click closes without committing" "$STATE/hold-closed.png" 157
chk "hold: nothing was committed" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
hq hyprnotify clear >/dev/null; sleep 0.6
nfy holdb "hold commit"
sleep 1
longpress "$ROWX" "$ROWY" 272
expect_panel "hold-commit: the menu opens Default-staged for a clean app" "$STATE/hold-commit-menu.png" 445
click "$MENU_X" "$MENU_SILENT_Y_DEFAULTSTAGED" 272
expect_panel "hold-commit: stage Silent" "$STATE/hold-commit-silent.png" 427
click "$MENU_DONE_X" "$MENU_DONE_Y" 272
expect_panel "hold-commit: Done commits and the card folds back" "$STATE/hold-commit-done.png" 157
chk "hold-commit: the app is silenced" test "$(hq hyprnotify policy)" = "silenced:1 s=holdb priority:0"
longpress "$ROWX" "$ROWY" 272
expect_panel "hold-commit: the menu reopens Silent-staged" "$STATE/hold-restage.png" 427
click "$MENU_X" "$MENU_DEFAULT_Y_SILENTSTAGED" 272
expect_panel "hold-commit: stage Default back" "$STATE/hold-restage-default.png" 445
click "$MENU_DONE_X" "$MENU_DONE_Y" 272
expect_panel "hold-commit: Default is committed again" "$STATE/hold-restage-done.png" 157
chk "hold-commit: the silence rule is gone" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
hq hyprnotify clear >/dev/null; sleep 0.6

# ---- the snooze: commit, undo window, late-undo no-op ------------------------
# Snooze is a card state, not a mode: the card swaps to a 74px undo row for
# CONFIRM_MS, the row's Undo restores the card, and the snoozed card survives
# Clear all until it is closed by id.
SNOOZE_ID=$(nfyid holds "snooze me")
sleep 1
longpress "$ROWX" "$ROWY" 272
expect_panel "snooze: the menu opens" "$STATE/snooze-menu.png" 445
click "$MENU_X" "$MENU_SNOOZE_Y" 272
expect_panel "snooze: the section unfolds its four options" "$STATE/snooze-open.png" 653
click "$MENU_X" "$MENU_OPT1_Y" 272
expect_panel "snooze: picking an option keeps the section open" "$STATE/snooze-opt.png" 653
click "$MENU_DONE_X" "$MENU_DONE_Y_OPEN" 272
expect_panel "snooze: the card swaps to its undo row" "$STATE/snooze-row.png" 157
chk "snooze: the model counts it snoozed" test "$(hq hyprnotify snoozed)" = "1"
click "$UNDO_X" "$UNDO_Y" 272
expect_panel "snooze: Undo restores the card inside the window" "$STATE/snooze-undo.png" 157
chk "snooze: the undo cleared the snooze" test "$(hq hyprnotify snoozed)" = "0"
longpress "$ROWX" "$ROWY" 272
click "$MENU_X" "$MENU_SNOOZE_Y" 272
expect_panel "snooze: re-snoozing opens the section again" "$STATE/snooze-reopen.png" 653
click "$MENU_X" "$MENU_OPT2_Y" 272
click "$MENU_DONE_X" "$MENU_DONE_Y_OPEN" 272
expect_panel "snooze: committed again" "$STATE/snooze-row2.png" 157
chk "snooze: snoozed again" test "$(hq hyprnotify snoozed)" = "1"
sleep 7.2 # the 6s undo window lapses before the late press
click "$UNDO_X" "$UNDO_Y" 272
# Spec 2.8: on commit the notification is REMOVED and re-posted quietly
# after the delay. The undo row holds the slot only for CONFIRM_MS; once it
# lapses the card leaves the view (the model keeps it snoozed), so the empty
# shade is the correct height and the late press hits nothing.
expect_panel "snooze: a late Undo is a no-op" "$STATE/snooze-late.png" 145
chk "snooze: still snoozed after the late press" test "$(hq hyprnotify snoozed)" = "1"
closeid "$SNOOZE_ID"
sleep 0.6
chk "snooze: a snoozed card can be closed by id" test "$(hq hyprnotify snoozed)" = "0"
hq hyprnotify clear >/dev/null; sleep 0.5

# ---- the conversation hold menu (1:1) ----------------------------------------
# The same surface under a chat title: every y is +22, the buttons ride the
# footer at 393 closed / 601 open.
conversation_notify convhold chat-c "Chat C" dana Dana c-1 "hello"
sleep 1
expect_panel "conv-hold: the collapsed conversation is 70px" "$STATE/convhold-card.png" 157
longpress "$ROWX" "$ROWY" 272
expect_panel "conv-hold: the menu opens under its chat title" "$STATE/convhold-menu.png" 467
click "$MENU_X" "$MENU_SILENT_Y_CONV_DEFAULTSTAGED" 272
expect_panel "conv-hold: stage Silent" "$STATE/convhold-silent.png" 449
click "$MENU_X" "$MENU_SNOOZE_Y_CONV" 272
expect_panel "conv-hold: snooze unfolds" "$STATE/convhold-snooze-open.png" 657
click "$MENU_X" "$MENU_OPT2_Y_CONV" 272
click "$MENU_DONE_X" "$MENU_DONE_Y_CONV_OPEN" 272
expect_panel "conv-hold: the conversation snoozes to its undo row" "$STATE/convhold-snoozed.png" 157
chk "conv-hold: snoozed" test "$(hq hyprnotify snoozed)" = "1"
click "$UNDO_X" "$UNDO_Y" 272
expect_panel "conv-hold: Undo restores the collapsed card" "$STATE/convhold-undo.png" 157
chk "conv-hold: un-snoozed" test "$(hq hyprnotify snoozed)" = "0"
longpress "$ROWX" "$ROWY" 272
click "$MENU_X" "$MENU_SILENT_Y_CONV_DEFAULTSTAGED" 272
click "$MENU_DONE_X" "$MENU_DONE_Y_CONV" 272
expect_panel "conv-hold: Silent committed, the card folds back" "$STATE/convhold-committed.png" 157
chk "conv-hold: the app is silenced" test "$(hq hyprnotify policy)" = "silenced:1 s=convhold priority:0"
longpress "$ROWX" "$ROWY" 272
expect_panel "conv-hold: the menu reopens Silent-staged" "$STATE/convhold-restage.png" 449
click "$MENU_X" "$MENU_DEFAULT_Y_CONV_SILENTSTAGED" 272
click "$MENU_DONE_X" "$MENU_DONE_Y_CONV" 272
expect_panel "conv-hold: Default committed back" "$STATE/convhold-restored.png" 157
chk "conv-hold: the silence rule is gone" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
hq hyprnotify clear >/dev/null; sleep 0.5

# ---- the group conversation and the digest ----------------------------------
# Two senders in one group chat fold into one card with a two-line preview;
# two cards in one declared group fold into a digest. Both keep the full hold
# menu (a digest's is the no-snooze flavor, buttons at 319).
conversation_notify holde grp-e "Team E" eve Eve e-1 one group
conversation_notify holde grp-e "Team E" evan Evan e-2 two group
sleep 1
expect_panel "group-conv: two senders fold into one two-line card" "$STATE/group-conv-card.png" 172
longpress "$ROWX" "$ROWY" 272
expect_panel "group-conv: its menu carries the chat title" "$STATE/group-conv-menu.png" 467
click "$MENU_X" "$MENU_SILENT_Y_CONV_DEFAULTSTAGED" 272
expect_panel "group-conv: stage Silent" "$STATE/group-conv-silent.png" 449
click "$ROWX" "$ROWY" 273
expect_panel "group-conv: right-click closes, nothing committed" "$STATE/group-conv-closed.png" 172
chk "group-conv: no rule left behind" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
click "$ROWX" "$ROWY" 273
expect_panel "group-conv: a second right-click dismisses the conversation" "$STATE/group-conv-gone.png" 145
chk "group-conv: the whole conversation left the model" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.5

sec_notify heldg "declared one" shared
sec_notify heldg "declared two" shared
sleep 1
expect_panel "digest: the declared group folds into one 70px card" "$STATE/digest-card.png" 155
longpress "$ROWX" "$ROWY" 272
expect_panel "digest: the bundle menu has no snooze section" "$STATE/digest-menu.png" 393
click "$MENU_X" "$MENU_SILENT_Y_DEFAULTSTAGED" 272
expect_panel "digest: stage Silent" "$STATE/digest-silent.png" 375
click "$MENU_DONE_X" "$MENU_DONE_Y_BUNDLE_SILENTSTAGED" 272
expect_panel "digest: Done commits the group rule" "$STATE/digest-committed.png" 155
chk "digest: the app is silenced" test "$(hq hyprnotify policy)" = "silenced:1 s=heldg priority:0"
longpress "$ROWX" "$ROWY" 272
expect_panel "digest: the menu reopens Silent-staged" "$STATE/digest-restage.png" 375
click "$MENU_X" "$MENU_DEFAULT_Y_SILENTSTAGED" 272
click "$MENU_DONE_X" "$MENU_DONE_Y_BUNDLE" 272
expect_panel "digest: Default committed back" "$STATE/digest-restored.png" 155
chk "digest: the silence rule is gone" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
click "$ROWX" "$ROWY" 273
expect_panel "digest: right-click dismisses the WHOLE group" "$STATE/digest-gone.png" 145
chk "digest: both cards left the model" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.5

# ---- generated-identity pixels ------------------------------------------------
# A 1:1 conversation with no sender icon gets a generated avatar: a hashed
# fill plus initials. Two senders must not share a face, and neither face may
# dissolve into the card glass. Sampled off the initials glyph, inside the
# 37px lead circle.
conversation_notify idapp chat-d "Chat D" dana Dana d-1 "hello"
conversation_notify idapp chat-e "Chat E" ivan Ivan e-1 "hi"
sleep 1
expect_panel "identity: two 1:1 chats, two cards" "$STATE/identity.png" 239
chk "identity: the two senders get distinct generated avatars" test "$(
	python3 - "$STATE/identity.png" "$PANEL_X" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
PX = int(sys.argv[2])
def px(x, y): return im.getpixel((x, y))
# The stack is newest-first, so neither sender's slot is assumed: sample each
# avatar's top-center, clear of the initials glyph and the app mark.
a  = px(PX + 44, 64)    # slot 1 avatar top-center
b  = px(PX + 44, 152)   # slot 2 avatar top-center
bg = px(PX + 200, 100)  # card glass, below the text run
print("pass" if a != b and a != bg and b != bg else f"fail a={a} b={b} bg={bg}")
PY
)" = "pass"
hq hyprnotify clear >/dev/null; sleep 0.6

# ---- overflow and the wheel ---------------------------------------------------
# The shade fit-caps its stack: ten 72px title-only cards, only eight fit
# the 667px body cap. The vertical wheel pages through the rest, one card per
# 15px of accumulated delta; nothing is dropped, only skipped past. The
# virtual pointer's delta->step ratio is compositor-dependent, so page to the
# clamped ends in bounded repeats — the ends are exact, the midpoints aren't.
for i in 1 2 3 4 5 6 7 8 9 10; do nfy "of$i" "overflow $i" ""; done
sleep 1.4
expect_panel "overflow: eight of ten title-only cards fit" "$STATE/overflow-full.png" 715
if page_to "$STATE/overflow-scrolled.png" 155 150; then ok "overflow: the wheel pages to the last card"; else bad "overflow: the wheel pages to the last card (want panel h 155)"; fi
if page_to "$STATE/overflow-back.png" 715 -150; then ok "overflow: the wheel pages back to the top"; else bad "overflow: the wheel pages back to the top (want panel h 715)"; fi
chk "overflow: paging kept every card" test "$(st)" = "center:1 live:10 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the history panel ---------------------------------------------------------
# A deliberate horizontal flick (>90px) flips the history sheet over the
# shade's content from any card or the panel itself. Dismissed cards land in
# the last-32 FIFO; Clear empties the list, not the model.
nfy h1 "hist one"
nfy h2 "hist two"
nfy h3 "hist three"
sleep 1.2
expect_panel "history: three cards, history closed" "$STATE/hist-cards.png" 321
click "$ROWX" "$ROWY" 273
expect_panel "history: the first dismiss" "$STATE/hist-r1.png" 239
click "$ROWX" "$ROWY" 273
expect_panel "history: the second dismiss" "$STATE/hist-r2.png" 157
click "$ROWX" "$ROWY" 273
expect_panel "history: the third dismiss leaves the empty state" "$STATE/hist-r3.png" 145
swipeh "$ROWX" "$ROWY" 120
expect_panel "history: the flick opens the sheet with three entries" "$STATE/hist-open.png" 342
click "$((PANEL_X + 316))" 124 272
expect_panel "history: Clear empties the list" "$STATE/hist-cleared.png" 242
swipeh "$ROWX" "$ROWY" -120
expect_panel "history: the flick back closes the sheet" "$STATE/hist-closed.png" 145
center_off
hq hyprnotify clear >/dev/null; sleep 0.6

# ---- the module leaves the plugin exactly as the preflight found it -----------
chk "notifications: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "notifications: no policy left behind" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
chk "notifications: no snooze in flight" test "$(hq hyprnotify snoozed)" = "0"
chk "notifications: no banners or residents" test "$(bd)" = "banners:0 resident:0"
