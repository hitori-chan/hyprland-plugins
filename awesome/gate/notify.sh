# awesome/gate/notify.sh — the notify module's behavior battery: the cap,
# expiry and residency, coalescing, the conversation merge, topline, overflow
# paging, the shade's click model, close-on-act, hover-hold, the bell
# (pointer-driven: hover is a no-op, a click toggles), keyboard nav,
# banners-over-fullscreen, the reply protocol, and the sound-spawn
# backpressure.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- notification cap ---------------------------------------------------
storm_jobs=()
for i in $(seq 1 65); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true; sleep 5
chk "notif storm: cap holds at exactly 50/65" test "$(hq awesome count)" = 50
chk "no history verb survives the model removal" test "$(hq awesome history)" = "unknown request"
chk "no recall verb survives the model removal" test "$(hq awesome recall)" = "unknown request"
hq awesome clear >/dev/null; sleep 0.8
# wrong-typed hints make sdbus-c++ throw inside the parse — the catch must
# survive (exercises exception unwinding across the .so boundary).
dsp "hl.dsp.exec_cmd('notify-send -h int:transient:1 -h string:urgency:critical typed-hint-abuse body')"; sleep 1.5
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" bash -c "hyprctl -i $SIG awesome count | grep -qE '^[0-9]+$'"
hq awesome clear >/dev/null 2>&1; sleep 0.6

# ---- expiry, residency & the center ------------------------------------
chk "notif reset: clean state line" test "$(st)" = "center:0 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a crit -u critical \"urgent\" body')"
sleep 1
chk "critical: a sticky banner, nothing kept yet" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.exec_cmd('notify-send -a norm -t 600 normal body')"
sleep 1.2
chk "residency: the expired normal banner retreated to a resident row" test "$(bd)" = "banners:1 resident:1"
chk "residency: the retreated card stays in the model, not lost" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a low -u low -t 600 lowcard body')"
dsp "hl.dsp.exec_cmd('notify-send -a tran -e -t 600 trans body')"
sleep 1.2
chk "ephemerals: transient vanished, low parked resident" test "$(st)" = "center:0 live:3 dnd:0"
chk "ephemerals: only the critical banner is still up" test "$(bd)" = "banners:1 resident:2"
hq awesome center >/dev/null; sleep 0.5
chk "center: opening absorbs the banner into the shade" test "$(bd)" = "banners:0 resident:3"
chk "center: opening dismisses nothing" test "$(st)" = "center:1 live:3 dnd:0"
hq awesome center >/dev/null; sleep 0.5
chk "center: closing neither re-pops nor drops a card" test "$(st)" = "center:0 live:3 dnd:0"
hq awesome clear >/dev/null; sleep 0.8
chk "center: Clear all sweeps every visible card" test "$(st)" = "center:0 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send \"default normal\" body')"
sleep 1
chk "default normal: pops as a banner" test "$(bd)" = "banners:1 resident:0"
sleep 5
chk "default normal: retreated to the shade after timeout_normal" test "$(bd)" = "banners:0 resident:1"
chk "default normal: the card is kept, not lost" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.8

# ---- popup coalescing: one live banner per app -------------------------
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
hq awesome clear >/dev/null; sleep 0.8

# ---- the conversation merge (Android's MessagingStyle) ------------------
for m in one two three; do dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Alice \"$m\"')"; sleep 0.4; done
sleep 0.8
chk "merge: 3 messages from one sender collapse to 1 card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Bob hello')"; sleep 1
chk "merge: a different sender keeps its own card" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain one\" body')"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain two\" body')"; sleep 1
chk "merge: no category, no merging — same app still stacks" test "$(st)" = "center:0 live:4 dnd:0"
hq awesome clear >/dev/null; sleep 0.8

# ---- the collapsed one-liner: the NEWEST message --------------------------
pcidb toplineapp "Carol" cid-t1 "first message"
sleep 1
pcidb toplineapp "Carol" cid-t1 "second message"
pcidb toplineapp "Carol" cid-t1 "third message"
sleep 1.2
chk "topline: a transcript card's one-liner is the newest message" test "$(hq awesome topline)" = "third message"
conv_notify toplineapp2 "Dave" "alpha"
conv_notify toplineapp2 "Dave" "beta"
sleep 1.2
chk "topline: a joined card's one-liner is the newest message" test "$(hq awesome topline)" = "beta"
hq awesome clear >/dev/null; sleep 0.8
tdgrp "Design Team" Alice "sketched the new logo"
sleep 0.6
tdgrp "Design Team" Bob "looks great, ship it"
tdgrp "Design Team" Alice "uploading the final files now"
sleep 1.2
chk "topline: the group card merged to one card" test "$(st)" = "center:0 live:1 dnd:0"
chk "topline: the group card's one-liner is the newest message, sender inline" test "$(hq awesome topline)" = "<b>Alice</b>: uploading the final files now"
hq awesome clear >/dev/null; sleep 0.8

# ---- shade overflow ------------------------------------------------------
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq awesome center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
wheel 120
wheel 120
chk "overflow: wheeling down keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8

# ---- the center hero: the shade's face of the banner preview -------------
HEROIMG=/tmp/hero-probe-aw.png
python3 - "$HEROIMG" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (640, 360), (30, 160, 240)).save(sys.argv[1])
PY
hero_px() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]) - 362, int(sys.argv[2]) - 16
n = 0
for y in range(46, 152):
    for x in range(x0, x1, 2):
        r, g, b = px[x, y]
        if b > 200 and r < 100 and 120 < g < 200:
            n += 1
print(n)
PY
}
hero_span() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]) - 362, int(sys.argv[2]) - 16
ys = [y for y in range(40, 170)
      if any(b > 200 and r < 100 and 120 < g < 200 for r, g, b in (px[x, y] for x in range(x0, x1, 8)))]
print(max(ys) - min(ys) + 1 if ys else 0)
PY
}
dsp "hl.dsp.exec_cmd('notify-send -a heroapp -t 2000 -i $HEROIMG \"Screenshot\" saved')"; sleep 1
chk "hero: the image card pops with its banner" test "$(bd)" = "banners:1 resident:0"
hq awesome center >/dev/null; sleep 0.4
capture_nested /tmp/hero-1-aw.png
chk "hero: the center row (open by default) shows the preview" test "$(hero_px /tmp/hero-1-aw.png)" -gt 8000
chk "hero: the preview is the capped strip, not the full image" test "$(hero_span /tmp/hero-1-aw.png)" -ge 104 -a "$(hero_span /tmp/hero-1-aw.png)" -le 116
PB1="$(panel_bottom /tmp/hero-1-aw.png)"
click $CHVX 174 272
capture_nested /tmp/hero-2-aw.png
chk "hero: the folded row keeps the preview" test "$(hero_px /tmp/hero-2-aw.png)" -gt 8000
PB2="$(panel_bottom /tmp/hero-2-aw.png)"
chk "hero: the panel follows the fold (the body height moves)" test $(( PB2 > PB1 ? PB2 - PB1 : PB1 - PB2 )) -ge 18
sleep 1.5
capture_nested /tmp/hero-3-aw.png
chk "hero: the preview survives the banner retreating" test "$(hero_px /tmp/hero-3-aw.png)" -gt 8000
chk "hero: the card is a resident, its banner is down" test "$(bd)" = "banners:0 resident:1"
click $CHVX 174 272
sleep 0.4
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8
chk "hero: reset after the center-hero battery" test "$(st)" = "center:0 live:0 dnd:0"

gen_hero() {
	python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (int(sys.argv[2]), int(sys.argv[3])), (30, 160, 240)).save(sys.argv[1])
PY
}
gen_hero /tmp/hero-sq-aw.png 420 420
dsp "hl.dsp.exec_cmd('notify-send -a herop -t 3000 -i /tmp/hero-sq-aw.png \"Screenshot\" region')"; sleep 1
hq awesome center >/dev/null; sleep 0.4
capture_nested /tmp/hero-4-aw.png
chk "hero: a square capture leads with the capped strip" test "$(hero_px /tmp/hero-4-aw.png)" -gt 8000 -a "$(hero_span /tmp/hero-4-aw.png)" -ge 104 -a "$(hero_span /tmp/hero-4-aw.png)" -le 116
gen_hero /tmp/hero-pt-aw.png 420 640
dsp "hl.dsp.exec_cmd('notify-send -a herop -t 3000 -i /tmp/hero-pt-aw.png \"Screenshot\" region')"; sleep 1
capture_nested /tmp/hero-5-aw.png
chk "hero: a portrait capture leads with the capped strip" test "$(hero_px /tmp/hero-5-aw.png)" -gt 8000 -a "$(hero_span /tmp/hero-5-aw.png)" -ge 104 -a "$(hero_span /tmp/hero-5-aw.png)" -le 116
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8
chk "hero: reset after the shape battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's click model ---------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"read me\" body')"; sleep 1
chk "shade: one card waiting" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null; sleep 0.6
click $CHVX $ROWY 272
chk "shade: the chevron only folds — nothing invoked, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $CHVX $ROWY 272
chk "shade: the chevron unfolds again, still nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "shade: left on an actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
nfyact gatechat "open me" 2 default Open 0; sleep 1
hq awesome center >/dev/null; sleep 0.6
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
nfyact gatechat "stay me" 2 default Open 1 resident b true; sleep 1
hq awesome center >/dev/null; sleep 0.6
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- hover holds a banner's clock ------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 1200 \"hold me\" body')"; sleep 0.4
capture_nested /tmp/banner-aw.png
chk "banner: it is on screen (the card's pixels)" test "$(banner_px /tmp/banner-aw.png)" -gt 150
printf 'move %s 64\nsleep 2400\n' "$POPX" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 0.4
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.4
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
hq awesome clear >/dev/null; sleep 0.8

# ---- the bell (pointer-driven) ------------------------------------------------
# The shade opens on a CLICK only: hover is a no-op (the hover-peek was
# removed by user request, 2026-09-30). The bell is a real widget — the
# hover and the click are real pointer events over the glyph, found by
# pixel.
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"ring me\" body')"; sleep 1
chk "bell: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
bell_frame || true
BELLX="$(bell_x "$STATE/bell-probe.png")"
chk "bell: the bell glyph is found in the strip" test "$BELLX" -gt 0
bell_hover true
chk "bell: hovering the bell is a no-op" test "$(st)" = "center:0 live:1 dnd:0"
chk "bell: hover leaves the banner a banner" test "$(bd)" = "banners:1 resident:0"
bell_hover false
chk "bell: the shade is still shut" test "$(st)" = "center:0 live:1 dnd:0"
printf 'move %s %s\nsleep 120\nmove %s 13\nsleep 60\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
    "$(( (BELLX + MON_W / 2) / 2 ))" "$(( MON_H / 2 ))" "$BELLX" | vp
sleep 1
chk "bell: a click opens the shade and absorbs the banner" test "$(st)" = "center:1 live:1 dnd:0"
chk "bell: the absorption parked the banner" test "$(bd)" = "banners:0 resident:1"
hq awesome center >/dev/null; sleep 0.5
chk "bell: the explicit close re-pops the parked banner" test "$(bd)" = "banners:1 resident:0"
hq awesome clear >/dev/null; sleep 0.8
chk "bell: reset after the bell battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's keyboard nav ----------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 60000 -A default=Open \"key one\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"key two\" body')"; sleep 1
hq awesome center >/dev/null; sleep 0.7
chk "keys: two rows with the shade open" test "$(st)" = "center:1 live:2 dnd:0"
tap down
chk "keys: down only SELECTS — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:2 dnd:0"
tap space
chk "keys: space only folds" test "$(st)" = "center:1 live:2 dnd:0"
tap delete
chk "keys: delete dismisses the selected row" test "$(st)" = "center:1 live:1 dnd:0"
tap enter
chk "keys: enter fires the primary, taking the card and the shade" test "$(st)" = "center:0 live:0 dnd:0"
hq awesome center >/dev/null; sleep 0.5
chk "keys: a fresh shade for esc to close" test "$(st)" = "center:1 live:0 dnd:0"
tap esc
chk "keys: esc closes the shade" test "$(st)" = "center:0 live:0 dnd:0"
# the shade's own pixels: opening with a card renders the panel body
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"shade px\" body')"; sleep 1
hq awesome center >/dev/null; sleep 0.6
capture_nested /tmp/center-px-aw.png
chk "shade: the opened shade has pixels below the strip" test "$(panel_bottom /tmp/center-px-aw.png)" -gt 100
hq awesome center >/dev/null; sleep 0.3
hq awesome clear >/dev/null; sleep 0.8

# ---- banners over fullscreen ------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x400')"; sleep 2
dsp "hl.dsp.window.fullscreen()"; sleep 1
dsp "hl.dsp.exec_cmd('notify-send -a q1 -t 30000 over-fullscreen body')"; sleep 1.2
expect "fs-banner: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
chk "fs-banner: the banner shows over the fullscreen window" test "$(bd)" = "banners:1 resident:0"
hq awesome clear >/dev/null; sleep 0.5
dsp "hl.dsp.window.close()"; sleep 1

# ---- bus: closing an unknown ID is an error --------------------------------
dsp "hl.dsp.exec_cmd('notify-send -a unk -t 30000 unknown \"close probe body\"')"; sleep 1.2
chk "bus-close: a live card for the probe" test "$(bd)" = "banners:1 resident:0"
chk "bus-close: unknown id is an error" bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 4294967000 2>&1 | grep -q 'Call failed: Unknown notification ID'"
chk "bus-close: the failed close touched nothing" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.5

# ---- swipe: the horizontal wheel on a row -----------------------------------
psend swiper "flick me" ""; sleep 1.2
hq awesome center >/dev/null; sleep 0.7
chk "swipe: a card in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
swipe $ROWMID_X 64 25
chk "swipe: away dismissed the row" test "$(st)" = "center:1 live:0 dnd:0"
tap esc; hq awesome clear >/dev/null; sleep 0.8
chk "swipe: reset after the swipe battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- ranking -----------------------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -e -t 60000 -a chat \"an ordinary card\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -a alarm -u critical \"disk failing\" body')"; sleep 1
hq awesome center >/dev/null; sleep 0.7
chk "ranking: an absorbed critical beside an unabsorbed transient" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "ranking: the TOP row was the critical, not the card that came first" test "$(bd)" = "banners:1 resident:0"
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8

# ---- absorb is idempotent -----------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send \"keep one\" body')"
dsp "hl.dsp.exec_cmd('notify-send \"keep two\" body')"; sleep 1
for i in 1 2 3; do hq awesome center >/dev/null; sleep 0.35; done
chk "absorb: three toggles leave the two cards intact" bash -c "hyprctl -i $SIG awesome state | grep -qE '^center:1 live:2 '"
hq awesome center >/dev/null; sleep 0.35
chk "absorb: the close re-popped the newest, the sibling parked (one per app)" test "$(bd)" = "banners:1 resident:1"
hq awesome clear >/dev/null; sleep 0.8

# ---- repop ---------------------------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"repop me\" body')"; sleep 1
chk "repop: a live banner before the open" test "$(bd)" = "banners:1 resident:0"
hq awesome center >/dev/null; sleep 0.7
chk "repop: the open absorbed it" test "$(bd)" = "banners:0 resident:1"
outside_click; sleep 0.8
chk "repop: the outside-click close popped it back" test "$(bd)" = "banners:1 resident:0"
hq awesome clear >/dev/null; sleep 0.8

# ---- DND -----------------------------------------------------------------------
dsp "hl.plugin.awesome.suspend()"; sleep 0.5
chk "DND arms" hq_matches 'dnd:1' awesome state
dsp "hl.dsp.exec_cmd('notify-send -a q one body')"
dsp "hl.dsp.exec_cmd('notify-send -a q two body')"; sleep 0.8
chk "DND: two same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.awesome.suspend()"; sleep 0.6
chk "DND resume: one popped, the sibling resumed resident (one per app)" test "$(bd)" = "banners:1 resident:1"
chk "DND resume: dnd off, both cards kept" hq_matches '^center:0 live:2 dnd:0$' awesome state
hq awesome clear >/dev/null; sleep 0.8

# ---- hostile hints ---------------------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -h int:category:5 \"badcat\" body')"
dsp "hl.dsp.exec_cmd('notify-send -h string:category:im.received \"convo\" body')"; sleep 1
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
hq awesome clear >/dev/null; sleep 0.8

# ---- inline reply -----------------------------------------------------------------
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED" 2>&1 & )
sleep 0.5
conv_reply_notify Telegram "Alice" "are you around?"
sleep 1
hq awesome center >/dev/null; sleep 0.7
chk "reply: the chat card is in the shade" test "$(st)" = "center:1 live:1 dnd:0"
tap down
tap tab
tap 35
tap 23
chk "reply: typing into the field neither acts nor dismisses" test "$(st)" = "center:1 live:1 dnd:0"
tap esc
chk "reply: esc drops the field and NOT the shade" test "$(st)" = "center:1 live:1 dnd:0"
tap tab
tap 35
tap 23
tap enter
sleep 0.6
chk "reply: enter sent it and the card went" test "$(st)" = "center:1 live:0 dnd:0"
chk "reply: NotificationReplied carried the typed text" grep -q 'STRING "hi"' "$REPLIED"
hq awesome center >/dev/null; sleep 0.4
hq awesome clear >/dev/null; sleep 0.8

# ---- sound-spawn backpressure ------------------------------------------------------
# The sound path is ARRIVAL-triggered (the sound-file/sound-name hints): a
# hostile sender with a steady stream of sound-hinted notifications would
# hold a steady fork rate. The hang fixture keeps each helper alive long
# enough to count them: 20 distinct-app arrivals, 16 admitted spawns.
: > "$STATE/hang-sound"
rm -f "$STATE/hang-sound.pid"
for i in $(seq 1 20); do
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "flood$i" 0 "" "sound" body 0 1 sound-name s gate 30000 >/dev/null 2>&1
done
sleep 1.5
chk "sound: the spawn cap holds" test "$(wc -l < "$STATE/hang-sound.pid" 2>/dev/null || echo 0)" -eq 16
chk "sound: the capped spawns still carried every card" test "$(st)" = "center:0 live:20 dnd:0"
xargs -r kill 2>/dev/null <"$STATE/hang-sound.pid"
rm -f "$STATE/hang-sound" "${STATE}/hang-sound.pid"
hq awesome clear >/dev/null; sleep 0.8
chk "sound: the flood cleared" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the module leaves the plugin exactly as the preflight found it --------
chk "notifications: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "notifications: no banners or residents" test "$(bd)" = "banners:0 resident:0"
