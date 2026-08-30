#!/usr/bin/env bash
# The hyprnotify behavior battery: the notification cap, expiry and residency,
# popup coalescing, the conversation merge, overflow paging, the shade's click
# model, close-on-act, hover-hold, the bell's hover-peek, keyboard nav, and
# quiet-while-fullscreen. Helpers live in notify-lib.sh; this file is battery
# code only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"

# The hostile-tsv battery (windows) relaunched the nested target with a
# well-formed keepme rule still standing: lift it before the first assertion,
# or every "clean state" check below passes over a live silence.
policy_lift
chk "notif reset: no rule left standing by the hostile fixture" test "$(pol)" = "silenced:0 priority:0"

# ---- notification cap ---------------------------------------------------
# The model is deliberately bounded: 65 arrivals leave exactly max_notifs
# (50) behind, and the shade has no history — the evicted cards are gone, and
# the verbs that used to resurrect them are gone with it.
for i in $(seq 1 65); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" &
done; wait; sleep 5
chk "notif storm: cap holds at exactly 50/65" test "$(hq hyprnotify count)" = 50
chk "no history verb survives the model removal" test "$(hq hyprnotify history)" = "unknown request"
chk "no recall verb survives the model removal" test "$(hq hyprnotify recall)" = "unknown request"
hq hyprnotify clear >/dev/null; sleep 0.8
# wrong-typed hints make sdbus-c++ throw inside the plugin's parse — the
# catch must survive (exercises exception unwinding across the .so boundary).
# Cards expire on their own clocks, so assert the daemon still answers with
# a number, not any absolute count.
dsp "hl.dsp.exec_cmd('notify-send -h int:transient:1 -h string:urgency:critical typed-hint-abuse body')"; sleep 1.5
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" bash -c "hyprctl -i $SIG hyprnotify count | grep -qE '^[0-9]+$'"
# that card still runs its own clock — drop it before the next battery
# asserts a clean state (the monolith got this clear from its relaunch)
hq hyprnotify clear >/dev/null 2>&1; sleep 0.6

# ---- expiry, residency & the center ------------------------------------
# A normal banner runs its clock, then RETREATS to a resident shade row —
# still in the model (the center is the safety net), just no longer a popup.
# Critical (urgency>=2) is the only sticky banner. Ephemerals vanish
# outright: transient. The `state` line counts residents as `live` (raw model
# size, blind to the popup/shade split); the `badge` verb reads that split —
# "banners:N resident:N". Distinct -a apps here so popup coalescing (tested
# below) can't interfere.
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
# A plain normal card is not sticky — it runs timeout_normal (5s) and
# retreats to the shade on its own, unattended.
dsp "hl.dsp.exec_cmd('notify-send \"default normal\" body')"
sleep 1
chk "default normal: pops as a banner" test "$(bd)" = "banners:1 resident:0"
sleep 5
chk "default normal: retreated to the shade after timeout_normal" test "$(bd)" = "banners:0 resident:1"
chk "default normal: the card is kept, not lost" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- popup coalescing: one live banner per app -------------------------
# Spam control (coalesce_popups, default on): while an app shows a banner,
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

# ---- the conversation merge (Android's MessagingStyle) ------------------
# One chat is ONE card however many messages arrive: a fresh Notify whose app
# + summary matches a live card is joined onto it. The fd.o conversation
# categories are the trigger (the summary is the sender), so a plain card
# from the same app must NOT be swallowed.
for m in one two three; do dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Alice \"$m\"')"; sleep 0.4; done
sleep 0.8
chk "merge: 3 messages from one sender collapse to 1 card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Bob hello')"; sleep 1
chk "merge: a different sender keeps its own card" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain one\" body')"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain two\" body')"; sleep 1
chk "merge: no category, no merging — same app still stacks" test "$(st)" = "center:0 live:4 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- shade overflow ------------------------------------------------------
# More rows than the monitor-tall panel holds must PAGE, not bleed off the
# bottom. 15 distinct-app cards -> 15 rows (one app each, so nothing
# coalesces); the expansion budget opens what fits and folds the rest.
# Drawing it (the placement break + the paging cue) must not crash and must
# keep every card.
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq hyprnotify center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
wheel 120
wheel 120
chk "overflow: wheeling down keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the shade's click model ---------------------------------------------
# A shade row IS its banner: left on the BODY fires the card's primary and
# dismisses it, and the CHEVRON is the only fold target. Driven through the
# real hit boxes via vptr. The panel hangs off the monitor's right edge
# (EDGE 10 + CENTER_W 360) below offset_y 34, so the first row's body is a
# fixed inset from the top-right corner, and the chevron rides the row's
# right end (ROW_PADX 12 + CHEV 24). Hit boxes are final-position, so the
# open spring cannot move them out from under the click. The fold has no
# model-level path, hence the shape of these assertions: the chevron must
# change NOTHING in the model, while the body clears the card.
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"read me\" body')"; sleep 1
chk "shade: one card waiting" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.6
click $CHVX $ROWY 272
chk "shade: the chevron only folds — nothing invoked, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $CHVX $ROWY 272
chk "shade: the chevron unfolds again, still nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
# The card carries no actions, so the body click has nothing to fire: it is
# a pure DISMISSAL, and dismissing never closes the shade (center stays 1).
chk "shade: left on an actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
# Firing a card's primary raises the sender over the very panel the click was
# made in, so the panel leaves with it — AOSP collapses the shade on a
# content-intent click. `resident` is the fd.o way of saying the action does
# NOT take you away, and it holds the shade exactly as it holds the card.
# nfyact, not notify-send, for the reason in notify-lib: busctl's call
# returns and leaves nothing behind — the card's whole life is hyprnotify's.
nfyact gatechat "open me" 2 default Open 0; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
nfyact gatechat "stay me" 2 default Open 1 resident b true; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- hover holds a banner's clock ------------------------------------------
# A card must not expire out from under the pointer reading it. The pointer
# parks on the popup (top-right: EDGE 10 + width 348, below offset_y 34) for
# longer than the card's own timeout, then leaves — which RESTARTS the full
# clock rather than resuming the sliver that was left.
dsp "hl.dsp.exec_cmd('notify-send -t 1200 \"hold me\" body')"; sleep 0.4
printf 'move %s 64\nsleep 2400\n' "$POPX" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 0.4
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.4
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the bell's hover-peek --------------------------------------------------
# Driven over the very bus verb hyprbar's bell calls. A peek opens the shade
# UNPINNED and must NOT absorb: a pointer crossing the bell cannot be allowed
# to swallow banners the user never read. Leaving closes it again after the
# grace; a toggle (the bell's click) pins instead of closing.
# Guard the whole battery against passing vacuously: if the call lands on the
# WRONG daemon (or none), every "the shade stayed shut" assertion below is
# true for the wrong reason.
chk "peek: the nested daemon is the one answering, and it has Peek" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify | grep -q '\.Peek'"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"peek me\" body')"; sleep 1
chk "peek: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
peek true
chk "peek: hovering the bell opens the shade" test "$(st)" = "center:1 live:1 dnd:0"
chk "peek: a peek does NOT absorb the banner" test "$(bd)" = "banners:1 resident:0"
peek false # > the 400ms grace
chk "peek: leaving the bell closes it again" test "$(st)" = "center:0 live:1 dnd:0"
peek true
hq hyprnotify center >/dev/null; sleep 0.5
chk "peek: the bell's click PINS rather than closing" test "$(st)" = "center:1 live:1 dnd:0"
chk "peek: pinning absorbs what the peek left alone" test "$(bd)" = "banners:0 resident:1"
peek false
chk "peek: a pinned shade ignores the pointer leaving" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "peek: reset after the peek battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's keyboard nav ----------------------------------------------
# The one surface with no pointer path at all. Injected through a REAL
# virtual keyboard so it rides the same emission a physical key does. The
# list is newest-first, so ↓ lands on "key two" and the card carrying the
# primary is behind it. The destructive steps are positive assertions on
# purpose: a dead injector would make every "nothing changed" line pass for
# the wrong reason.
dsp "hl.dsp.exec_cmd('notify-send -t 60000 -A default=Open \"key one\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"key two\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "keys: two rows with the shade open" test "$(st)" = "center:1 live:2 dnd:0"
tap down
chk "keys: down only SELECTS — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:2 dnd:0"
tap space
chk "keys: space only folds" test "$(st)" = "center:1 live:2 dnd:0"
tap delete
chk "keys: delete dismisses the selected row" test "$(st)" = "center:1 live:1 dnd:0"
tap enter
# Enter is the body click's twin, so it collapses for the same reason: "key
# one" carries a real -A default, and firing it raises the sender over the
# panel. The card goes AND the shade goes with it.
chk "keys: enter fires the primary, taking the card and the shade" test "$(st)" = "center:0 live:0 dnd:0"
# Which leaves esc nothing to close — give it its own shade, or the line
# below passes on a shade that was already gone.
hq hyprnotify center >/dev/null; sleep 0.5
chk "keys: a fresh shade for esc to close" test "$(st)" = "center:1 live:0 dnd:0"
tap esc
chk "keys: esc closes the shade" test "$(st)" = "center:0 live:0 dnd:0"

# ---- quiet while fullscreen -------------------------------------------------
# A real fullscreen window owns the screen, so the banner is held back and
# the card lands straight in the shade (residency is the safety net, so
# nothing is lost). Critical still punches through, exactly as it does
# through DND.
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x400')"; sleep 2
dsp "hl.dsp.window.fullscreen()"; sleep 1
# Mode 2 is FSMODE_FULLSCREEN; 1 is merely maximized and must NOT count
expect "quiet-fs: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
dsp "hl.dsp.exec_cmd('notify-send -a q1 -t 30000 quiet body')"; sleep 1.2
chk "quiet-fs: the card landed silent, no banner" test "$(bd)" = "banners:0 resident:1"
dsp "hl.dsp.exec_cmd('notify-send -a q2 -u critical \"loud\" body')"; sleep 1.2
chk "quiet-fs: critical still punches through" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.5
dsp "hl.dsp.window.fullscreen()"; sleep 1
dsp "hl.dsp.exec_cmd('notify-send -a q3 -t 30000 loudagain body')"; sleep 1.2
chk "quiet-fs: out of fullscreen, banners are back" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; sleep 0.5
dsp "hl.dsp.window.close()"; sleep 1

# ---- the module leaves the plugin exactly as the preflight found it --------
chk "notifications: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "notifications: no policy left behind" test "$(hq hyprnotify policy)" = "silenced:0 priority:0"
chk "notifications: no snooze in flight" test "$(hq hyprnotify snoozed)" = "0"
chk "notifications: no banners or residents" test "$(bd)" = "banners:0 resident:0"
