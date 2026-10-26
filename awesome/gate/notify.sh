# awesome/gate/notify.sh — the notify module's behavior battery: the cap,
# expiry and residency, coalescing, the conversation merge, topline, overflow
# paging, the hero preview, the shade's click model, close-on-act,
# hover-hold, the bell click, keyboard nav, banners-over-fullscreen, repop,
# DND, the reply protocol, the sound-spawn backpressure, and transient cards.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- notification cap ---------------------------------------------------
storm_jobs=()
for i in $(seq 1 65); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true
until_is 8 50 hq awesome count; sleep 0.5 # and nothing past the cap lands late
chk "notif storm: cap holds at exactly 50/65" test "$(hq awesome count)" = 50
nclear
# wrong-typed hints make sdbus-c++ throw inside the parse — the catch must
# survive (exercises exception unwinding across the .so boundary).
dsp "hl.dsp.exec_cmd('notify-send -h int:transient:1 -h string:urgency:critical typed-hint-abuse body')"; sleep 1.5
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" bash -c "hyprctl -i $SIG awesome count | grep -qE '^[0-9]+$'"
nclear

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
capture_nested "$STATE/center-head.png"
chk "center: the heading names the column, left of its controls" \
	test "$(lit_px "$STATE/center-head.png" $((PANEL_X + 4)) $((PANEL_X + 150)) $((N_TOP + 4)) $((N_TOP + 24)))" -gt 80
hq awesome center >/dev/null; sleep 0.5
chk "center: closing neither re-pops nor drops a card" test "$(st)" = "center:0 live:3 dnd:0"
nclear
chk "center: Clear all sweeps every visible card" test "$(st)" = "center:0 live:0 dnd:0"

# ---- popup coalescing: coalesce_popups (3) live banners per app ----------
dsp "hl.dsp.exec_cmd('notify-send -a chatty -t 30000 first body')"; sleep 1
chk "coalesce: the first same-app arrival pops a banner" test "$(bd)" = "banners:1 resident:0"
for m in second third fourth; do dsp "hl.dsp.exec_cmd('notify-send -a chatty -t 30000 $m body')"; done; sleep 1
chk "coalesce: the app's fourth card lands resident, three banners stand" test "$(bd)" = "banners:3 resident:1"
chk "coalesce: every card is kept and counted" test "$(st)" = "center:0 live:4 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a chatty -u critical urgent body')"; sleep 1
chk "coalesce: a critical from the same app punches through" test "$(bd)" = "banners:4 resident:1"
dsp "hl.dsp.exec_cmd('notify-send -a other elsewhere body')"; sleep 1
chk "coalesce: a different app gets its own banner" test "$(bd)" = "banners:5 resident:1"
nclear

# ---- the conversation merge (Android's MessagingStyle) ------------------
for m in one two three; do dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Alice \"$m\"')"; sleep 0.4; done
sleep 0.8
chk "merge: 3 messages from one sender collapse to 1 card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Bob hello')"; sleep 1
chk "merge: a different sender keeps its own card" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain one\" body')"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain two\" body')"; sleep 1
chk "merge: no category, no merging — same app still stacks" test "$(st)" = "center:0 live:4 dnd:0"
nclear

# ---- the collapsed one-liner: the NEWEST message --------------------------
pcidb toplineapp "Carol" cid-t1 "first message"
sleep 1
pcidb toplineapp "Carol" cid-t1 "second message"
pcidb toplineapp "Carol" cid-t1 "third message"
sleep 1.2
chk "topline: a transcript card's one-liner is the newest message" test "$(hq awesome topline)" = "third message"
nclear
tdgrp "Design Team" Alice "sketched the new logo"
sleep 0.6
tdgrp "Design Team" Bob "looks great, ship it"
tdgrp "Design Team" Alice "uploading the final files now"
sleep 1.2
chk "topline: the group card merged to one card" test "$(st)" = "center:0 live:1 dnd:0"
chk "topline: the group card's one-liner is the newest message, sender inline" test "$(hq awesome topline)" = "<b>Alice</b>: uploading the final files now"
nclear

# ---- shade overflow ------------------------------------------------------
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq awesome center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
wheel 120
wheel 120
chk "overflow: wheeling down keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq awesome center >/dev/null; sleep 0.4
nclear

# ---- the hero: the banner's preview, the shade row's thumbnail -----------
HEROIMG=$STATE/hero-probe-aw.png
python3 - "$HEROIMG" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (1280, 720), (30, 160, 240)).save(sys.argv[1]) # past 512²: scaled on the raster worker
PY
hero_px() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]) - 12 - 356, int(sys.argv[2]) - 12
n = 0
for y in range(40, 280):
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
x0, x1 = int(sys.argv[2]) - 12 - 356, int(sys.argv[2]) - 12
ys = [y for y in range(40, 280)
      if any(b > 200 and r < 100 and 120 < g < 200 for r, g, b in (px[x, y] for x in range(x0, x1, 8)))]
print(max(ys) - min(ys) + 1 if ys else 0)
PY
}
dsp "hl.dsp.exec_cmd('notify-send -a heroapp -t 2000 -i $HEROIMG \"Screenshot\" saved')"; sleep 1
chk "hero: the image card pops with its banner" test "$(bd)" = "banners:1 resident:0"
capture_nested $STATE/hero-0-aw.png
chk "hero: the banner leads with the card-wide preview" test "$(hero_px $STATE/hero-0-aw.png)" -gt 8000
# a 1280x720 image at the card's 328 px is 184 tall: HERO_CAP holds it to 160
chk "hero: the banner's preview is the capped strip, not the full image" test "$(hero_span $STATE/hero-0-aw.png)" -ge 150 -a "$(hero_span $STATE/hero-0-aw.png)" -le 166
hq awesome center >/dev/null; sleep 0.4
capture_nested $STATE/hero-1-aw.png
chk "hero: the shade row shows no card-wide preview" test "$(hero_px $STATE/hero-1-aw.png)" -lt 3000
chk "hero: the shade row leads with the image's thumbnail" test "$(hero_px $STATE/hero-1-aw.png)" -gt 200
chk "hero: the thumbnail is icon-sized" test "$(hero_span $STATE/hero-1-aw.png)" -ge 30 -a "$(hero_span $STATE/hero-1-aw.png)" -le 50
hq awesome center >/dev/null; sleep 0.4
chk "hero: the explicit close re-pops the banner" test "$(bd)" = "banners:1 resident:0"
capture_nested $STATE/hero-3-aw.png
chk "hero: the re-popped banner shows the preview again" test "$(hero_px $STATE/hero-3-aw.png)" -gt 8000
nclear
chk "hero: reset after the center-hero battery" test "$(st)" = "center:0 live:0 dnd:0"
# the same preview as raw image-data: unpacked and scaled on the worker
DBUS_SESSION_BUS_ADDRESS="$NBUS" python3 - <<'PY'
from gi.repository import Gio, GLib
W, H = 1280, 720
i32 = GLib.Variant.new_int32
data = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), GLib.Bytes.new(bytes((30, 160, 240)) * (W * H)), True)
img = GLib.Variant.new_tuple(i32(W), i32(H), i32(W * 3), GLib.Variant.new_boolean(False), i32(8), i32(3), data)
args = GLib.Variant.new_tuple(GLib.Variant("s", "heroapp"), GLib.Variant("u", 0), GLib.Variant("s", ""), GLib.Variant("s", "Screenshot"),
                              GLib.Variant("s", "pixels"), GLib.Variant("as", []), GLib.Variant("a{sv}", {"image-data": img}), i32(2000))
Gio.bus_get_sync(Gio.BusType.SESSION).call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                               "Notify", args, GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 5000, None)
PY
sleep 1
capture_nested $STATE/hero-4-aw.png
chk "hero: an image-data screenshot previews card-wide" test "$(hero_px $STATE/hero-4-aw.png)" -gt 8000
chk "hero: the image-data preview is the capped strip" test "$(hero_span $STATE/hero-4-aw.png)" -ge 150 -a "$(hero_span $STATE/hero-4-aw.png)" -le 166
nclear

# ---- the Notify reader (notify/bus.cpp) ---------------------------------------
# The call is read by hand: image-data's pixels leave the message in one
# block once its header checks out, a fixed-type array under any other key
# is passed over without a copy. The reply's round trip times the dispatch
# (the typed read took 0.65 s for a 1080p frame).
notify_raw() { # notify_raw <hint key> <w> <h> <stride|auto> — prints the reply's ms
	DBUS_SESSION_BUS_ADDRESS="$NBUS" python3 - "$@" <<'PY'
import sys, time
from gi.repository import Gio, GLib
key, W, H, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
i32 = GLib.Variant.new_int32
raw = GLib.Bytes.new(bytes((30, 160, 240, 255)) * (W * H))
if key.startswith("x-"):
    hint = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), raw, True)
else:
    data = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), raw, True)
    hint = GLib.Variant.new_tuple(i32(W), i32(H), i32(W * 4 if stride == "auto" else int(stride)), GLib.Variant.new_boolean(True), i32(8), i32(4), data)
args = GLib.Variant.new_tuple(GLib.Variant("s", "heroapp"), GLib.Variant("u", 0), GLib.Variant("s", ""), GLib.Variant("s", "Screenshot"),
                              GLib.Variant("s", "raw"), GLib.Variant("as", []), GLib.Variant("a{sv}", {key: hint}), i32(4000))
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
t0 = time.monotonic()
bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
              "Notify", args, GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 10000, None)
print(int((time.monotonic() - t0) * 1000))
PY
}
MS="$(notify_raw image-data 1920 1080 auto)"; echo "reader: 1080p image-data answered in ${MS} ms" >&2
chk "reader: a 1080p image-data answers within 250 ms" test "${MS:-9999}" -lt 250
sleep 1
capture_nested $STATE/reader-1-aw.png
chk "reader: and previews card-wide" test "$(hero_px $STATE/reader-1-aw.png)" -gt 8000
nclear
MS="$(notify_raw x-gate-junk 1920 1080 auto)"; echo "reader: an 8 MB unknown hint answered in ${MS} ms" >&2
chk "reader: an 8 MB array under an unknown key is passed over (within 250 ms)" test "${MS:-9999}" -lt 250
chk "reader: its card landed" until_is 3 "center:0 live:1 dnd:0" st
nclear
notify_raw icon_data 1280 720 auto >/dev/null; sleep 1
capture_nested $STATE/reader-2-aw.png
chk "reader: the spec 1.0 name (icon_data) still previews" test "$(hero_px $STATE/reader-2-aw.png)" -gt 8000
nclear
notify_raw image-data 1280 720 0 >/dev/null; sleep 1
capture_nested $STATE/reader-3-aw.png
chk "reader: a lying header (stride 0) lands the card" test "$(st)" = "center:0 live:1 dnd:0"
chk "reader: with no preview" test "$(hero_px $STATE/reader-3-aw.png)" -lt 200
nclear
# an fd array under a hint (sd-bus reads no fd array as a block): passed
# over like any other value, the card still lands
DBUS_SESSION_BUS_ADDRESS="$NBUS" python3 - <<'PY'
import os
from gi.repository import Gio, GLib
V = GLib.Variant
r, w = os.pipe()
fds = Gio.UnixFDList.new()
i = fds.append(r)
os.close(r); os.close(w)
args = V.new_tuple(V("s", "fdapp"), V("u", 0), V("s", ""), V("s", "fds"), V("s", "fd array"), V("as", []), V("a{sv}", {"x-fds": V("ah", [i])}), V.new_int32(4000))
Gio.bus_get_sync(Gio.BusType.SESSION).call_with_unix_fd_list_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                                                  "Notify", args, GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 5000, fds, None)
PY
chk "reader: an fd array under a hint still lands the card" until_is 3 "center:0 live:1 dnd:0" st
nclear
# shapes past the fast path: a value wrapped in containers is walked past
# without a copy; a big-endian sender's image-data (sd-bus serves it no
# block) is read byte by byte up to 256 KiB, refused past it (a header
# declaring more before the walk, a lying one at the bound); a walk past
# its element budget is refused, never a stall
notify_shape() { # notify_shape wrapped|be-small|be-big|be-lying|walk-past — prints "<ms> ok|err"
	DBUS_SESSION_BUS_ADDRESS="$NBUS" python3 - "$1" <<'PY'
import sys, time
from gi.repository import Gio, GLib
V, i32, shape = GLib.Variant, GLib.Variant.new_int32, sys.argv[1]
def ay(n): return V.new_from_bytes(GLib.VariantType.new("ay"), GLib.Bytes.new(bytes((30, 160, 240, 255)) * (n // 4)), True)
def img(w, n=0): return V.new_tuple(i32(w), i32(w), i32(w * 4), V.new_boolean(True), i32(8), i32(4), ay(n or w * w * 4))
hints = {"wrapped": {"x-wrapped": V("v", ay(8 << 20))}, "be-small": {"image-data": img(256)}, "be-big": {"image-data": img(1024)},
         "be-lying": {"image-data": img(16, 1 << 20)},
         "walk-past": {"x-many": V("as", ["x"] * 100000)}}[shape]
msg = Gio.DBusMessage.new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify")
msg.set_body(V.new_tuple(V("s", "shapeapp"), V("u", 0), V("s", ""), V("s", "shape"), V("s", shape), V("as", []), V("a{sv}", hints), i32(4000)))
if shape.startswith("be-"):
    msg.set_byte_order(Gio.DBusMessageByteOrder.BIG_ENDIAN)
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
blob = msg.to_blob(Gio.DBusCapabilityFlags.NONE)  # serialize before the clock starts
t0 = time.monotonic()
reply, _ = bus.send_message_with_reply_sync(msg, Gio.DBusSendMessageFlags.NONE, 10000, None)
print(int((time.monotonic() - t0) * 1000), "err" if reply.get_message_type() == Gio.DBusMessageType.ERROR else "ok")
PY
}
read -r MS R <<<"$(notify_shape wrapped)"; echo "reader: 8 MB in two variants answered in ${MS} ms ($R)" >&2
chk "reader: an 8 MB array in two variants is walked past (within 250 ms)" test "$R" = ok -a "${MS:-9999}" -lt 250
nclear
read -r MS R <<<"$(notify_shape be-small)"; sleep 1
capture_nested $STATE/reader-4-aw.png
chk "reader: a big-endian sender's image-data previews" test "$R" = ok -a "$(hero_px $STATE/reader-4-aw.png)" -gt 8000
nclear
read -r MS R <<<"$(notify_shape be-big)"; echo "reader: a 4 MiB big-endian image-data answered in ${MS} ms ($R)" >&2
chk "reader: a big-endian image-data past 256 KiB is refused, not stalled" test "$R" = err -a "${MS:-9999}" -lt 250
read -r MS R <<<"$(notify_shape be-lying)"; echo "reader: a 16x16 header over 1 MiB, big-endian, answered in ${MS} ms ($R)" >&2
chk "reader: and so is one whose header understates it" test "$R" = err -a "${MS:-9999}" -lt 250
read -r MS R <<<"$(notify_shape walk-past)"; echo "reader: 100k strings answered in ${MS} ms ($R)" >&2
chk "reader: a hint past the walk budget is refused, not stalled" test "$R" = err -a "${MS:-9999}" -lt 250
DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications \
	org.freedesktop.Notifications Notify susssasa{sv}i after 0 "" "after" "still serving" 0 0 4000 >/dev/null
chk "reader: and the daemon still serves" until_is 3 "center:0 live:1 dnd:0" st
nclear

# ---- a spread stack keeps to the page ----------------------------------------
# 30 three-line cards of one app: spread, they would run far past the
# screen; the page shows the cards that fit and counts the rest
for i in $(seq 1 30); do
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications Notify \
		susssasa{sv}i bulk 0 "" "build $i" $'step one\nstep two\nstep three' 0 0 30000 >/dev/null
done
sleep 0.5
hq awesome center >/dev/null; sleep 0.6
chk "stack: thirty cards wait in the center" test "$(st)" = "center:1 live:30 dnd:0"
click $ROWX $ROWY 272
capture_nested $STATE/stack30-aw.png
# spread, it runs past mid-screen and still ends on it
chk "stack: the spread stack ends on the screen" test "$(panel_bottom $STATE/stack30-aw.png)" -gt $((MON_H / 2)) -a "$(panel_bottom $STATE/stack30-aw.png)" -lt $((MON_H - 4))
chk "stack: spreading acted on nothing" test "$(st)" = "center:1 live:30 dnd:0"
hq awesome center >/dev/null; sleep 0.4
nclear

# ---- the fallback face: listed by the helper, then worn ----------------------
# An iconless card starts the listing (a helper process, never the loop) and
# wears the generic mark until it lands; the warm it triggers re-rolls the
# card onto the one face in the dir.
FACES=$STATE/faces
mkdir -p "$FACES"
python3 - "$FACES/face.png" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (64, 64), (250, 120, 20)).save(sys.argv[1])
PY
face_px() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
n = 0
for y in range(30, 200):
    for x in range(int(sys.argv[2]) - 362, int(sys.argv[2]) - 16):
        r, g, b = px[x, y]
        if r > 220 and 90 < g < 150 and b < 60:
            n += 1
print(n)
PY
}
hq eval "hl.config({ plugin = { awesome = { notify = { fallback_icon_dir = \"$FACES\" } } } })" >/dev/null
dsp "hl.dsp.exec_cmd('notify-send -t 4000 faceless \"no icon\"')"; sleep 1.2
capture_nested $STATE/face-aw.png
chk "face: an iconless card wears the listed face" test "$(face_px $STATE/face-aw.png)" -gt 200
hq eval "hl.config({ plugin = { awesome = { notify = { fallback_icon_dir = \"\" } } } })" >/dev/null
nclear

# ---- the shade's click model ---------------------------------------------
# A folded card opens on a click before it acts. The top card opens by
# itself, so the keys fold it first; four lines fold to one.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications Notify \
	susssasa{sv}i gate 0 "" "read me" $'line one\nline two\nline three\nline four' 0 0 30000 >/dev/null
sleep 1
chk "shade: one card waiting" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null; sleep 0.6
tap down
tap space
capture_nested $STATE/shade-fold-aw.png
PB1="$(panel_bottom $STATE/shade-fold-aw.png)"
click $ROWX $ROWY 272
chk "shade: a click on a folded card only opens it — nothing invoked, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
capture_nested $STATE/shade-open-aw.png
PB2="$(panel_bottom $STATE/shade-open-aw.png)"
chk "shade: the opened card shows its whole body" test $(( PB2 - PB1 )) -ge 30
printf 'move %s %s\nsleep 40\npress 274\nsleep 40\nrelease 274\nsleep 80\n' "$ROWX" "$ROWY" | vp; sleep 0.35
chk "shade: middle on a row does nothing" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "shade: left on an actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null; sleep 0.4
nclear
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
nclear
nfyact_done
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- no buttons ride a card: the hold is its menu ---------------------------
# macOS's hover close button and Options are left out (the hold and the
# right click do their work): under the pointer the card's corner stays
# bare, and the held card menu opens at the pointer — near the right edge it
# flips left of it, its rows running down from the pointer's y under its
# x - 100 (notify/menu.cpp: PAD 5, rows 28, separators 11) — the sender's
# actions first.
hold_menu() { printf 'move %s %s\nsleep 40\npress 272\nsleep 700\n' "$1" "$2" | vp; sleep 0.3; } # the button stays down
release_left() { printf 'release 272\nsleep 80\n' | vp; sleep 0.3; }
mrow() { echo $(($1 + 5 + 14 + 28 * $2 + 11 * $3)); } # mrow <menu y> <row> <separators above>
corner_px() { # corner_px <frame> — near-white pixels where macOS's close button would ride
	python3 - "$1" "$PANEL_X" "$N_TOP" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x, y = int(sys.argv[2]), int(sys.argv[3])
print(sum(1 for yy in range(y - 7, y + 13) for xx in range(x - 7, x + 13) if min(px[xx, yy]) > 170))
PY
}
HX=$POPX HY=$((N_TOP + 22))
# the pointer off the column first: the battery before left it on a card
printf 'move %s %s\nsleep 60\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
dsp "hl.dsp.exec_cmd('notify-send -a hov -t 30000 \"hover me\" body')"; sleep 1
capture_nested $STATE/bare-rest.png
printf 'move %s %s\nsleep 200\n' "$HX" "$((N_TOP + 30))" | vp; sleep 0.4
capture_nested $STATE/bare-hover.png
R0=$(corner_px $STATE/bare-rest.png) R1=$(corner_px $STATE/bare-hover.png)
echo "bare: corner lit $R0 -> $R1" >&2
chk "bare: hover puts no close button on the card's corner" test "$R1" -le $((R0 + 2))
nclear
INVOKED="$STATE/invoked.log"
rm -f "$INVOKED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 30 busctl --user monitor --match "type='signal',member='ActionInvoked'" >"$INVOKED" 2>&1 & )
sleep 0.4
nfyact actapp "act on me" 4 default Open later "Remind Me" 0; sleep 1
hold_menu "$HX" "$HY"; release_left
chk "actions: the hold opens the menu and acts on nothing" test "$(st)" = "center:0 live:1 dnd:0"
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
chk "actions: the action's row fired it, and the card went" until_is 3 "center:0 live:0 dnd:0" st
chk "actions: ActionInvoked carried the action's key" grep -q 'STRING "later"' "$INVOKED"
nfyact_done
nclear

# ---- the center stacks an app's cards (macOS) --------------------------------
# Two cards of one app are one stack: collapsed, the newest over its deck
# (8 px a layer); a click spreads it under a header (Show Less and a close
# button, 24 px, right-aligned: notify/stack.cpp); right dismisses it whole.
dsp "hl.dsp.exec_cmd('notify-send -a stk -t 30000 first body')"; sleep 0.3
dsp "hl.dsp.exec_cmd('notify-send -a stk -t 30000 second body')"; sleep 1
hq awesome center >/dev/null; sleep 0.6
capture_nested $STATE/stack-deck.png
PD="$(panel_bottom $STATE/stack-deck.png)"
click $ROWX $ROWY 272
chk "stack: a click spreads it and acts on nothing" test "$(st)" = "center:1 live:2 dnd:0"
capture_nested $STATE/stack-spread.png
PS="$(panel_bottom $STATE/stack-spread.png)"
echo "stack: bottom collapsed $PD, spread $PS" >&2
chk "stack: spread, its header and both cards stand" test $((PS - PD)) -ge 40
click $((PANEL_X + N_W - 24 - 6 - 30)) $((CARD_Y + 12)) 272
capture_nested $STATE/stack-less.png
chk "stack: Show Less folds it back" test "$(panel_bottom $STATE/stack-less.png)" -le $((PD + 2))
click $ROWX $ROWY 273
chk "stack: right on the deck dismisses the whole stack" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null; sleep 0.4
nclear

# ---- hover holds a banner's clock ------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 1200 \"hold me\" body')"; sleep 0.4
capture_nested $STATE/banner-aw.png
chk "banner: it is on screen (the card's pixels)" test "$(banner_px $STATE/banner-aw.png)" -gt 150
printf 'move %s %s\nsleep 2400\n' "$POPX" "$((N_TOP + 20))" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 0.4
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.4
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
nclear

# ---- the card menu: hold left ------------------------------------------------
# A hold opens the card's switches at the pointer (hold_menu, mrow above); a
# chat leads with its two mute rows and a separator (these chats offer no
# actions, so no Reply or action rows come first). The switches persist as
# state.tsv rows.
pcidb gatechat Alice alice "are you coming"; sleep 1
chk "menu: a chat banner is up" test "$(bd)" = "banners:1 resident:0"
hold_menu "$HX" "$HY"
chk "menu: the hold fired no click" test "$(st)" = "center:0 live:1 dnd:0"
release_left
chk "menu: releasing on its corner picks nothing" test "$(bd)" = "banners:1 resident:0"
tap esc
click "$HX" "$HY" 272
chk "menu: esc closed it (the next click acts on the card)" test "$(st)" = "center:0 live:0 dnd:0"
pcidb gatechat Alice alice "one"; sleep 1
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 2 1)" 272
chk "menu: Turn Off Banners lowered the app's banner" until_is 3 "banners:0 resident:1" bd
pcidb gatechat Bob bob "ping"; sleep 0.8
chk "menu: the app's next arrival lands silent" test "$(bd)" = "banners:0 resident:2"
chk "menu: the switch persisted" grep -aqx $'notify-app\t1\tgatechat' "$AW_STATE"
# back on from the center: the menu opens down-right of a pointer that
# leaves it room
hq awesome center >/dev/null; sleep 0.6
hold_menu "$ROWX" "$ROWY"; release_left
click $((ROWX + 100)) "$(mrow "$ROWY" 2 1)" 272
chk "menu: Turn On from the center drops the row" bash -c "! grep -aq '^notify-app' '$AW_STATE'"
chk "menu: a pick acts on no card" test "$(st)" = "center:1 live:2 dnd:0"
hq awesome center >/dev/null; sleep 0.4
nclear
# a chat's mute is the chat's alone
pcidb gatechat Alice alice "one"; sleep 1
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
chk "menu: Mute for 1 Hour lowered the chat" until_is 3 "banners:0 resident:1" bd
pcidb gatechat Alice alice "two"; sleep 0.8
chk "menu: the muted chat's next message lands silent" test "$(bd)" = "banners:0 resident:1"
pcidb gatechat Bob bob "hi"; sleep 0.8
chk "menu: another chat of the app still banners" test "$(bd)" = "banners:1 resident:1"
chk "menu: the mute persisted" grep -aq $'^notify-mute\t[0-9]*\tgatechat\x1falice$' "$AW_STATE"
nclear
pcidb gatechat Alice alice "three"; sleep 0.8
hq awesome center >/dev/null; sleep 0.6
hold_menu "$ROWX" "$ROWY"; release_left
click $((ROWX + 100)) "$(mrow "$ROWY" 0 0)" 272
chk "menu: Unmute drops the row" bash -c "! grep -aq '^notify-mute' '$AW_STATE'"
hq awesome center >/dev/null; sleep 0.4
nclear
# press, hold, slide onto the last row, release: Close All from the app
# the middle banner is a mailer's whichever way the stack runs
dsp "hl.dsp.exec_cmd('notify-send -a other -t 30000 \"stays\" body')"; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a mailer -t 30000 \"report\" body')"; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a mailer -t 30000 \"disk\" body')"; sleep 1
chk "menu: three banners from two apps" test "$(bd)" = "banners:3 resident:0"
MX=$POPX MY=$((N_TOP + 68 + 22))
hold_menu "$MX" "$MY"
printf 'move %s %s\nsleep 120\n' $((MX - 100)) "$(mrow "$MY" 2 1)" | vp
release_left
chk "menu: slide-release Close All took only that app's cards" until_is 3 "center:0 live:1 dnd:0" st
nclear

# ---- the bell (pointer-driven) ------------------------------------------------
# The shade opens on a CLICK only: hover is a no-op (the hover-peek was
# removed by user request, 2026-09-30). The bell is a real widget — the
# hover and the click are real pointer events over it (lib.sh bell_x).
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"ring me\" body')"; sleep 1
chk "bell: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
bell_frame || true
BELLX="$(bell_x "$STATE/bell-probe.png")"
chk "bell: the bell is found in the strip" test "$BELLX" -gt 0
printf 'move %s %s\nsleep 120\nmove %s 13\nsleep 60\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
    "$(( (BELLX + MON_W / 2) / 2 ))" "$(( MON_H / 2 ))" "$BELLX" | vp
sleep 1
chk "bell: a click opens the shade and absorbs the banner" test "$(st)" = "center:1 live:1 dnd:0"
chk "bell: the absorption parked the banner" test "$(bd)" = "banners:0 resident:1"
hq awesome center >/dev/null; sleep 0.5
chk "bell: the explicit close re-pops the parked banner" test "$(bd)" = "banners:1 resident:0"
nclear
chk "bell: reset after the bell battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's keyboard nav ----------------------------------------------
# two apps: one app's two cards would stack, and the keys would act on the stack
dsp "hl.dsp.exec_cmd('notify-send -a keya -t 60000 -A default=Open \"key one\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -a keyb -t 60000 \"key two\" body')"; sleep 1
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
# ---- banners over fullscreen ------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x400')"; sleep 2
dsp "hl.dsp.window.fullscreen()"; sleep 1
dsp "hl.dsp.exec_cmd('notify-send -a q1 -t 30000 over-fullscreen body')"; sleep 1.2
expect "fs-banner: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
chk "fs-banner: the banner shows over the fullscreen window" test "$(bd)" = "banners:1 resident:0"
nclear
dsp "hl.dsp.window.close()"; sleep 1

# ---- bus: closing an unknown ID is an error --------------------------------
dsp "hl.dsp.exec_cmd('notify-send -a unk -t 30000 unknown \"close probe body\"')"; sleep 1.2
chk "bus-close: a live card for the probe" test "$(bd)" = "banners:1 resident:0"
chk "bus-close: unknown id is an error" bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 4294967000 2>&1 | grep -q 'Call failed: Unknown notification ID'"
chk "bus-close: the failed close touched nothing" test "$(st)" = "center:0 live:1 dnd:0"
nclear

# ---- repop ---------------------------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"repop me\" body')"; sleep 1
chk "repop: a live banner before the open" test "$(bd)" = "banners:1 resident:0"
hq awesome center >/dev/null; sleep 0.7
chk "repop: the open absorbed it" test "$(bd)" = "banners:0 resident:1"
outside_click; sleep 0.8
chk "repop: the outside-click close popped it back" test "$(bd)" = "banners:1 resident:0"
nclear

# ---- DND -----------------------------------------------------------------------
dsp "hl.plugin.awesome.suspend()"; sleep 0.5
chk "DND arms" hq_matches 'dnd:1' awesome state
for m in one two three four; do dsp "hl.dsp.exec_cmd('notify-send -a q $m body')"; done; sleep 0.8
chk "DND: four same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.awesome.suspend()"; sleep 0.6
chk "DND resume: three popped, the fourth resumed resident (the per-app cap)" test "$(bd)" = "banners:3 resident:1"
chk "DND resume: dnd off, every card kept" hq_matches '^center:0 live:4 dnd:0$' awesome state
nclear

# ---- DND holds a merge onto an existing card ------------------------------------
# A conversation's first message retreats to the shade; with DND armed the
# next message merges onto that card — it must not pop the banner DND is
# holding back.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i dndchat 0 "" Alice one 0 3 desktop-entry s dndchat category s im.received conversation-id s cid-dnd 600 >/dev/null 2>&1
until_is 3 "banners:0 resident:1" bd
dsp "hl.plugin.awesome.suspend()"; sleep 0.4
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i dndchat 0 "" Alice two 0 3 desktop-entry s dndchat category s im.received conversation-id s cid-dnd 30000 >/dev/null 2>&1
sleep 1
chk "DND: a merge onto a shaded card stays in the shade" test "$(bd)" = "banners:0 resident:1"
chk "DND: the merge landed on the one card" test "$(st)" = "center:0 live:1 dnd:1"
dsp "hl.plugin.awesome.suspend()"; sleep 0.4
nclear

# ---- hostile hints ---------------------------------------------------------------
dsp "hl.dsp.exec_cmd('notify-send -h int:category:5 \"badcat\" body')"
dsp "hl.dsp.exec_cmd('notify-send -h string:category:im.received \"convo\" body')"; sleep 1
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
nclear

# ---- inline reply -----------------------------------------------------------------
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED" 2>&1 & )
sleep 0.5
conv_reply_notify Telegram "Alice" "gone already"
sleep 1
hq awesome center >/dev/null; sleep 0.7
tap down
tap tab
tap esc
chk "reply: a card whose sender left the bus opens no field (esc closes the shade)" test "$(st)" = "center:0 live:1 dnd:0"
nclear
chk "reply: PyGObject for a sender that stays on the bus" python3 -c 'from gi.repository import Gio'
conv_reply_sender Telegram "Alice" "are you around?"
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
kill "$REPLY_SENDER" 2>/dev/null; wait "$REPLY_SENDER" 2>/dev/null
hq awesome center >/dev/null; sleep 0.4
nclear

# Reply from a banner's menu (its first row): the field opens in the banner,
# which holds its clock and takes the typing; Enter sends from there
REPLIED2="$STATE/replied-banner.log"
rm -f "$REPLIED2"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED2" 2>&1 & )
sleep 0.5
conv_reply_sender Telegram "Alice" "are you around?"
sleep 1
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
tap 35
tap 23
chk "reply: a banner's field takes the typing and acts on nothing" test "$(st)" = "center:0 live:1 dnd:0"
tap enter
sleep 0.6
chk "reply: enter sent it from the banner and the card went" test "$(st)" = "center:0 live:0 dnd:0"
chk "reply: NotificationReplied carried the banner's text" grep -q 'STRING "hi"' "$REPLIED2"
kill "$REPLY_SENDER" 2>/dev/null; wait "$REPLY_SENDER" 2>/dev/null
nclear

# ---- sound-spawn backpressure ------------------------------------------------------
# The sound path is ARRIVAL-triggered (the sound-file/sound-name hints): a
# hostile sender with a steady stream of sound-hinted notifications would
# hold a steady fork rate. The hang fixture keeps each helper alive long
# enough to count them: 20 distinct-app arrivals, 4 admitted spawns (the
# SOUND class budget — its own, so a flood never takes the volume keys'
# slots).
: > "$STATE/hang-sound"
rm -f "$STATE/hang-sound.pid"
for i in $(seq 1 20); do
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "flood$i" 0 "" "sound" body 0 1 sound-name s gate 30000 >/dev/null 2>&1
done
sleep 1.5
chk "sound: the spawn cap holds" test "$(wc -l < "$STATE/hang-sound.pid" 2>/dev/null || echo 0)" -eq 4
chk "sound: the capped spawns still carried every card" test "$(st)" = "center:0 live:20 dnd:0"
# every SOUND slot is held: the volume keys still have theirs
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.5
chk "sound: a sound flood leaves the volume keys their own slots" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
xargs -r kill 2>/dev/null <"$STATE/hang-sound.pid"
rm -f "$STATE/hang-sound" "${STATE}/hang-sound.pid"
nclear
chk "sound: the flood cleared" test "$(st)" = "center:0 live:0 dnd:0"

# ---- a transient card is feedback, never a shade row ------------------------
# A screenshot tool's preview: on screen while it lasts, never history. The
# shade opening over it ends it rather than parking it as a row.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "shot" 0 "" "Screenshot" "copied" 0 1 transient b true 30000 >/dev/null 2>&1
sleep 0.6
chk "transient: the card is live" test "$(st)" = "center:0 live:1 dnd:0"
chk "transient: no banner or resident on the badge" test "$(bd)" = "banners:0 resident:0"
hq awesome center >/dev/null; sleep 0.4
chk "transient: opening the shade ends the card" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null; sleep 0.4
chk "transient: closing the shade brings nothing back" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the module leaves the plugin exactly as the preflight found it --------
chk "notifications: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "notifications: no banners or residents" test "$(bd)" = "banners:0 resident:0"
