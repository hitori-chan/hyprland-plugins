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
	nsend -u $u "stress $i" body & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true
# every sender was answered: nothing past the cap can land late
chk "notif storm: cap holds at exactly 50/65" test "$(hq awesome count)" = 50
nclear
# wrong-typed hints make sdbus-c++ throw inside the parse — the catch must
# survive (exercises exception unwinding across the .so boundary).
nsend -h int:transient:1 -h string:urgency:critical typed-hint-abuse body
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" bash -c "hyprctl -i $SIG awesome count | grep -qE '^[0-9]+$'"
nclear

# ---- expiry, residency & the center ------------------------------------
chk "notif reset: clean state line" test "$(st)" = "center:0 live:0 dnd:0"
nsend -a crit -u critical "urgent" body
chk "critical: a sticky banner, nothing kept yet" test "$(bd)" = "banners:1 resident:0"
nsend -a norm -t 600 normal body
chk "residency: the expired normal banner retreated to a resident row" until_is 3 "banners:1 resident:1" bd
chk "residency: the retreated card stays in the model, not lost" test "$(st)" = "center:0 live:2 dnd:0"
nsend -a low -u low -t 600 lowcard body
nsend -a tran -e -t 600 trans body
chk "ephemerals: transient vanished, low parked resident" until_is 3 "center:0 live:3 dnd:0" st
chk "ephemerals: only the critical banner is still up" test "$(bd)" = "banners:1 resident:2"
hq awesome center >/dev/null
chk "center: opening absorbs the banner into the shade" test "$(bd)" = "banners:0 resident:3"
chk "center: opening dismisses nothing" test "$(st)" = "center:1 live:3 dnd:0"
capture_nested "$STATE/center-head.png"
chk "center: the heading names the column, at its top" \
	test "$(lit_px "$STATE/center-head.png" $((PANEL_X + 4)) $((PANEL_X + 150)) $((HEAD_Y + 4)) $((HEAD_Y + 24)))" -gt 80
hq awesome center >/dev/null
chk "center: closing neither re-pops nor drops a card" test "$(st)" = "center:0 live:3 dnd:0"
nclear
chk "center: Clear all sweeps every visible card" test "$(st)" = "center:0 live:0 dnd:0"

# ---- popup coalescing: coalesce_popups (3) live banners per app ----------
nsend -a chatty -t 30000 first body
chk "coalesce: the first same-app arrival pops a banner" test "$(bd)" = "banners:1 resident:0"
for m in second third fourth; do nsend -a chatty -t 30000 $m body; done
chk "coalesce: the app's fourth card lands resident, three banners stand" test "$(bd)" = "banners:3 resident:1"
chk "coalesce: every card is kept and counted" test "$(st)" = "center:0 live:4 dnd:0"
nsend -a chatty -u critical urgent body
chk "coalesce: a critical from the same app punches through" test "$(bd)" = "banners:4 resident:1"
nsend -a other elsewhere body
chk "coalesce: a different app gets its own banner" test "$(bd)" = "banners:5 resident:1"
nclear

# ---- the conversation merge (Android's MessagingStyle) ------------------
for m in one two three; do nsend -a tg -c im.received -t 30000 Alice "$m"; done
chk "merge: 3 messages from one sender collapse to 1 card" test "$(st)" = "center:0 live:1 dnd:0"
nsend -a tg -c im.received -t 30000 Bob hello
chk "merge: a different sender keeps its own card" test "$(st)" = "center:0 live:2 dnd:0"
nsend -a tg -t 30000 "plain one" body
nsend -a tg -t 30000 "plain two" body
chk "merge: no category, no merging — same app still stacks" test "$(st)" = "center:0 live:4 dnd:0"
nclear

# ---- the collapsed one-liner: the NEWEST message --------------------------
pcidb toplineapp "Carol" cid-t1 "first message"
pcidb toplineapp "Carol" cid-t1 "second message"
pcidb toplineapp "Carol" cid-t1 "third message"
chk "topline: a transcript card's one-liner is the newest message" until_is 3 "third message" hq awesome topline
nclear
tdgrp "Design Team" Alice "sketched the new logo"
tdgrp "Design Team" Bob "looks great, ship it"
tdgrp "Design Team" Alice "uploading the final files now"
chk "topline: the group card merged to one card" test "$(st)" = "center:0 live:1 dnd:0"
chk "topline: the group card's one-liner is the newest message, sender inline" until_is 3 "<b>Alice</b>: uploading the final files now" hq awesome topline
nclear

# ---- shade overflow ------------------------------------------------------
for i in $(seq 1 15); do nsend -a ovf$i -t 30000 "row $i" body; done
hq awesome center >/dev/null
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
wheel 120
wheel 120
chk "overflow: wheeling down keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq awesome center >/dev/null
nclear

# ---- the hero: the banner's preview, the shade row's thumbnail -----------
HEROIMG=$STATE/hero-probe-aw.png
python3 - "$HEROIMG" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (1280, 720), (30, 160, 240)).save(sys.argv[1]) # past 512²: scaled on the raster worker
PY
# hero_px/hero_span <frame> <y0> <y1>: the band a capture holds its card in
# (the banner column's top, or the shade's first card under the control
# center's block); one band per scan keeps each scan short
BANNER_BAND="$N_TOP 280" SHADE_BAND="$CARD_Y $((CARD_Y + 240))"
hero_px() {
	python3 - "$1" "$MON_W" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]) - 12 - 356, int(sys.argv[2]) - 12
n = 0
for y in range(int(sys.argv[3]), int(sys.argv[4])):
    for x in range(x0, x1, 2):
        r, g, b = px[x, y]
        if b > 200 and r < 100 and 120 < g < 200:
            n += 1
print(n)
PY
}
# hero_in <frame> <y0> <y1> <px over> <px under> [<span min> <span max>]
hero_in() {
	local n s
	n="$(hero_px "$1" "$2" "$3")"
	((n > $4 && n < $5)) || return 1
	[[ -z "${6:-}" ]] && return 0
	s="$(hero_span "$1" "$2" "$3")"
	((s >= $6 && s <= $7))
}
hero_span() {
	python3 - "$1" "$MON_W" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]) - 12 - 356, int(sys.argv[2]) - 12
ys = [y for y in range(int(sys.argv[3]), int(sys.argv[4]))
      if any(b > 200 and r < 100 and 120 < g < 200 for r, g, b in (px[x, y] for x in range(x0, x1, 8)))]
print(max(ys) - min(ys) + 1 if ys else 0)
PY
}
# its clock outlives the capture and the scans before the center opens
# (~1.5 s): a banner that expired first is resident, and nothing re-pops
nsend -a heroapp -t 6000 -i $HEROIMG "Screenshot" saved
chk "hero: the image card pops with its banner" until_is 3 "banners:1 resident:0" bd
shot_until 3 $STATE/hero-0-aw.png hero_in $STATE/hero-0-aw.png $BANNER_BAND 8000 999999 150 166
chk "hero: the banner leads with the card-wide preview" test "$(hero_px $STATE/hero-0-aw.png $BANNER_BAND)" -gt 8000
# a 1280x720 image at the card's 328 px is 184 tall: HERO_CAP holds it to 160
chk "hero: the banner's preview is the capped strip, not the full image" test "$(hero_span $STATE/hero-0-aw.png $BANNER_BAND)" -ge 150 -a "$(hero_span $STATE/hero-0-aw.png $BANNER_BAND)" -le 166
hq awesome center >/dev/null
shot_until 3 $STATE/hero-1-aw.png hero_in $STATE/hero-1-aw.png $SHADE_BAND 200 3000 30 50
chk "hero: the shade row shows no card-wide preview" test "$(hero_px $STATE/hero-1-aw.png $SHADE_BAND)" -lt 3000
chk "hero: the shade row leads with the image's thumbnail" test "$(hero_px $STATE/hero-1-aw.png $SHADE_BAND)" -gt 200
chk "hero: the thumbnail is icon-sized" test "$(hero_span $STATE/hero-1-aw.png $SHADE_BAND)" -ge 30 -a "$(hero_span $STATE/hero-1-aw.png $SHADE_BAND)" -le 50
hq awesome center >/dev/null
chk "hero: the explicit close re-pops the banner" test "$(bd)" = "banners:1 resident:0"
shot_until 3 $STATE/hero-3-aw.png hero_in $STATE/hero-3-aw.png $BANNER_BAND 8000 999999
chk "hero: the re-popped banner shows the preview again" test "$(hero_px $STATE/hero-3-aw.png $BANNER_BAND)" -gt 8000
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
shot_until 3 $STATE/hero-4-aw.png hero_in $STATE/hero-4-aw.png $BANNER_BAND 8000 999999 150 166
chk "hero: an image-data screenshot previews card-wide" test "$(hero_px $STATE/hero-4-aw.png $BANNER_BAND)" -gt 8000
chk "hero: the image-data preview is the capped strip" test "$(hero_span $STATE/hero-4-aw.png $BANNER_BAND)" -ge 150 -a "$(hero_span $STATE/hero-4-aw.png $BANNER_BAND)" -le 166
nclear
# a square is a face at any size: Firefox posts a page's icon at its own
# size (a 512 px app icon, a contact's avatar), and as a hero it was cut
# into a card-wide strip of its middle
DBUS_SESSION_BUS_ADDRESS="$NBUS" python3 - <<'PY'
from gi.repository import Gio, GLib
W = 512
i32 = GLib.Variant.new_int32
data = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), GLib.Bytes.new(bytes((30, 160, 240, 255)) * (W * W)), True)
img = GLib.Variant.new_tuple(i32(W), i32(W), i32(W * 4), GLib.Variant.new_boolean(True), i32(8), i32(4), data)
args = GLib.Variant.new_tuple(GLib.Variant("s", "Firefox"), GLib.Variant("u", 0), GLib.Variant("s", ""), GLib.Variant("s", "GitHub"),
                              GLib.Variant("s", "you were mentioned"), GLib.Variant("as", ["default", "Activate"]),
                              GLib.Variant("a{sv}", {"image-data": img, "desktop-entry": GLib.Variant("s", "firefox")}), i32(5000))
Gio.bus_get_sync(Gio.BusType.SESSION).call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                               "Notify", args, GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 5000, None)
PY
shot_until 3 $STATE/hero-5-aw.png hero_in $STATE/hero-5-aw.png $BANNER_BAND 200 3000
chk "hero: a square page icon is no card-wide preview" test "$(hero_px $STATE/hero-5-aw.png $BANNER_BAND)" -lt 3000
chk "hero: it leads the card as its face" test "$(hero_px $STATE/hero-5-aw.png $BANNER_BAND)" -gt 200
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
shot_until 3 $STATE/reader-1-aw.png hero_in $STATE/reader-1-aw.png $BANNER_BAND 8000 999999
chk "reader: and previews card-wide" test "$(hero_px $STATE/reader-1-aw.png $BANNER_BAND)" -gt 8000
nclear
MS="$(notify_raw x-gate-junk 1920 1080 auto)"; echo "reader: an 8 MB unknown hint answered in ${MS} ms" >&2
chk "reader: an 8 MB array under an unknown key is passed over (within 250 ms)" test "${MS:-9999}" -lt 250
chk "reader: its card landed" until_is 3 "center:0 live:1 dnd:0" st
nclear
notify_raw icon_data 1280 720 auto >/dev/null
shot_until 3 $STATE/reader-2-aw.png hero_in $STATE/reader-2-aw.png $BANNER_BAND 8000 999999
chk "reader: the spec 1.0 name (icon_data) still previews" test "$(hero_px $STATE/reader-2-aw.png $BANNER_BAND)" -gt 8000
nclear
notify_raw image-data 1280 720 0 >/dev/null; sleep 1
capture_nested $STATE/reader-3-aw.png
chk "reader: a lying header (stride 0) lands the card" test "$(st)" = "center:0 live:1 dnd:0"
chk "reader: with no preview" test "$(hero_px $STATE/reader-3-aw.png $BANNER_BAND)" -lt 200
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
read -r MS R <<<"$(notify_shape be-small)"
shot_until 3 $STATE/reader-4-aw.png hero_in $STATE/reader-4-aw.png $BANNER_BAND 200 999999
# square, as the 256 KiB bound keeps it: the card's face, not a preview
chk "reader: a big-endian sender's image-data shows" test "$R" = ok -a "$(hero_px $STATE/reader-4-aw.png $BANNER_BAND)" -gt 200
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
hq awesome center >/dev/null
chk "stack: thirty cards wait in the center" test "$(st)" = "center:1 live:30 dnd:0"
click $ROWX $ROWY 272
on_page() { local b; b="$(panel_bottom "$1")"; ((b > MON_H / 2 && b < MON_H - 4)); }
shot_until 3 $STATE/stack30-aw.png on_page $STATE/stack30-aw.png
# spread, it runs past mid-screen and still ends on it
chk "stack: the spread stack ends on the screen" test "$(panel_bottom $STATE/stack30-aw.png)" -gt $((MON_H / 2)) -a "$(panel_bottom $STATE/stack30-aw.png)" -lt $((MON_H - 4))
chk "stack: spreading acted on nothing" test "$(st)" = "center:1 live:30 dnd:0"
hq awesome center >/dev/null
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
nsend -t 4000 faceless "no icon"
faced() { (($(face_px "$1") > 200)); }
shot_until 3 $STATE/face-aw.png faced $STATE/face-aw.png
chk "face: an iconless card wears the listed face" test "$(face_px $STATE/face-aw.png)" -gt 200
hq eval "hl.config({ plugin = { awesome = { notify = { fallback_icon_dir = \"\" } } } })" >/dev/null
nclear

# ---- the shade's click model ---------------------------------------------
# A folded card opens on a click before it acts. The top card opens by
# itself, so the keys fold it first; four lines fold to one.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications Notify \
	susssasa{sv}i gate 0 "" "read me" $'line one\nline two\nline three\nline four' 0 0 30000 >/dev/null
chk "shade: one card waiting" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null
tap down
tap space
capture_nested $STATE/shade-fold-aw.png
PB1="$(panel_bottom $STATE/shade-fold-aw.png)"
click $ROWX $ROWY 272
chk "shade: a click on a folded card only opens it — nothing invoked, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
capture_nested $STATE/shade-open-aw.png
PB2="$(panel_bottom $STATE/shade-open-aw.png)"
chk "shade: the opened card shows its whole body" test $(( PB2 - PB1 )) -ge 30
printf 'move %s %s\nsleep 40\npress 274\nsleep 40\nrelease 274\nsleep 80\n' "$ROWX" "$ROWY" | vp; settle
chk "shade: middle on a row does nothing" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "shade: left on an actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
nsend -t 30000 "right me" body
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null
nclear
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
nfyact gatechat "open me" 2 default Open 0; sent_wait
hq awesome center >/dev/null
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
nfyact gatechat "stay me" 2 default Open 1 resident b true; sent_wait
hq awesome center >/dev/null
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq awesome center >/dev/null
nclear
nfyact_done
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- no buttons ride a card: the hold is its menu ---------------------------
# A card grows no hover close button or Options (the hold and the
# right click do their work): under the pointer the card's corner stays
# bare, and the held card menu opens at the pointer — near the right edge it
# flips left of it, its rows running down from the pointer's y under its
# x - 100 (notify/menu.cpp: PAD 5, rows 28, separators 11) — the sender's
# actions first.
menu_up() { [[ -n "$(hq awesome menu)" ]]; }
hold_menu() { printf 'move %s %s\nsleep 40\npress 272\n' "$1" "$2" | vp; until_ok 2 menu_up; } # the button stays down
release_left() { printf 'release 272\nsleep 80\n' | vp; settle; }
mrow() { echo $(($1 + 5 + 14 + 28 * $2 + 11 * $3)); } # mrow <menu y> <row> <separators above>
corner_px() { # corner_px <frame> — near-white pixels where a hover close button would ride
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
nsend -a hov -t 30000 "hover me" body
bannered() { (($(banner_px "$1") > 150)); }
shot_until 3 $STATE/bare-rest.png bannered $STATE/bare-rest.png
printf 'move %s %s\nsleep 200\n' "$HX" "$((N_TOP + 30))" | vp; settle
capture_nested $STATE/bare-hover.png
R0=$(corner_px $STATE/bare-rest.png) R1=$(corner_px $STATE/bare-hover.png)
echo "bare: corner lit $R0 -> $R1" >&2
chk "bare: hover puts no close button on the card's corner" test "$R1" -le $((R0 + 2))
nclear
INVOKED="$STATE/invoked.log"
rm -f "$INVOKED"
bus_watch "$INVOKED" "type='signal',member='ActionInvoked'"
nfyact actapp "act on me" 4 default Open later "Remind Me" 0; sent_wait
hold_menu "$HX" "$HY"; release_left
chk "actions: the hold opens the menu and acts on nothing" test "$(st)" = "center:0 live:1 dnd:0"
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
chk "actions: the action's row fired it, and the card went" until_is 3 "center:0 live:0 dnd:0" st
chk "actions: ActionInvoked carried the action's key" grep -q 'STRING "later"' "$INVOKED"
nfyact_done
nclear

# ---- the center stacks an app's cards ----------------------------------------
# Two cards of one app are one stack: collapsed, the newest over its deck
# (8 px a layer); a click spreads it under a 28 px header (the app's name,
# Show Less flush right: notify/stack.cpp); right dismisses it whole.
# the app's silent card folds apart: the stack's dismissal never takes it
nsend -a stk -u low -t 30000 hushstk body
nsend -a stk -t 30000 first body
nsend -a stk -t 30000 second body
hq awesome center >/dev/null
# the deck's measure once two frames agree on it (the shade warms in)
deck_steady() { local b; b="$(panel_bottom "$1")"; ((b > 0)) && [[ "$b" == "${PD_LAST:-}" ]] || { PD_LAST=$b; false; }; }
shot_until 3 $STATE/stack-deck.png deck_steady $STATE/stack-deck.png
PD="$(panel_bottom $STATE/stack-deck.png)"
click $ROWX $ROWY 272
chk "stack: a click spreads it and acts on nothing" test "$(st)" = "center:1 live:3 dnd:0"
bottom_over() { (($(panel_bottom "$1") > $2)); }
bottom_under() { (($(panel_bottom "$1") < $2)); }
shot_until 3 $STATE/stack-spread.png bottom_over $STATE/stack-spread.png $((PD + 39))
PS="$(panel_bottom $STATE/stack-spread.png)"
echo "stack: bottom collapsed $PD, spread $PS" >&2
chk "stack: spread, its header and both cards stand" test $((PS - PD)) -ge 40
click $((PANEL_X + N_W - 30)) $((CARD_Y + 14)) 272
shot_until 3 $STATE/stack-less.png bottom_under $STATE/stack-less.png $((PD + 3))
chk "stack: Show Less folds it back" test "$(panel_bottom $STATE/stack-less.png)" -le $((PD + 2))
click $ROWX $ROWY 273
chk "stack: right on the deck dismisses the whole stack, not the app's silent card" until_is 3 "- | silent:1" rows
hq awesome center >/dev/null
nclear

# ---- the center's ranking, its silent fold -----------------------------------
# Critical, then the chats, then the rest, 16 px apart where a group ends;
# the silent (low urgency, or silenced by the card menu: below) fold into
# one 44 px row at the bottom until a click spreads them for the visit
# (notify/center.cpp).
nsend -a rka -t 30000 plainrk body
nsend -a rkb -u low -t 30000 hushrk body
nsend -a rkc -c im.received -t 30000 chatrk body
nsend -a rkd -u critical urgentrk body
hq awesome center >/dev/null
chk "rank: critical, the chat, the rest, the silent folded" until_is 3 "- | !urgentrk @chatrk -plainrk silent:1" rows
shot_until 3 $STATE/rank.png bottom_over $STATE/rank.png $((CARD_Y + 200))
FB="$(panel_bottom $STATE/rank.png)"
click "$ROWX" $((FB - 22)) 272
chk "rank: a click on the fold spreads it" until_is 3 "- | !urgentrk @chatrk -plainrk ~hushrk" rows
chk "rank: ...acting on nothing" test "$(st)" = "center:1 live:4 dnd:0"

# Clear All hides the rows for 5 s and reads Undo meanwhile; Undo brings
# them back; the window's end, or the center's close, closes them
CLX=$((PANEL_X + N_W - 20)) CLY=$((HEAD_Y + 14))
click $CLX $CLY 272
chk "undo: Clear All hides every row at once" until_is 3 "- undo |" rows
chk "undo: ...closing none yet" test "$(st)" = "center:1 live:4 dnd:0"
chk "undo: ...nor counting them on the bell" test "$(bd)" = "banners:0 resident:0"
click $CLX $CLY 272
chk "undo: Undo brings every row back" until_is 3 "- | !urgentrk @chatrk -plainrk ~hushrk" rows
click $CLX $CLY 272
chk "undo: the window's end closes them for good" until_is 8 "center:1 live:0 dnd:0" st
nsend -a rka -t 30000 again body
click $CLX $CLY 272
chk "undo: an arrival is hidden by the next Clear All" until_is 3 "- undo |" rows
hq awesome center >/dev/null
chk "undo: closing the center closes them at once" until_is 2 "center:0 live:0 dnd:0" st

# a card the center had not shown wears its time in the accent for the
# visit; seen, it reads plain at the next
nsend -a fresh -t 30000 freshrk body
hq awesome center >/dev/null
fresh_lit() { (($(accent_px "$1" $((PANEL_X + N_W - 90)) $((PANEL_X + N_W - 10)) $((CARD_Y + 6)) $((CARD_Y + 34))) >= 8)); }
fresh_dim() {
	(($(accent_px "$1" $((PANEL_X + N_W - 90)) $((PANEL_X + N_W - 10)) $((CARD_Y + 6)) $((CARD_Y + 34))) == 0)) &&
		(($(lit_px "$1" $((PANEL_X + 50)) $((PANEL_X + 200)) $((CARD_Y + 8)) $((CARD_Y + 34))) > 40))
}
chk "fresh: an unseen card's time wears the accent" shot_until 3 $STATE/fresh.png fresh_lit $STATE/fresh.png
hq awesome center >/dev/null
until_is 2 "center:0 live:1 dnd:0" st
hq awesome center >/dev/null
chk "fresh: seen, its time reads plain at the next visit" shot_until 3 $STATE/fresh2.png fresh_dim $STATE/fresh2.png
hq awesome center >/dev/null
nclear

# a group chat with no picture wears its initials on a flat colour of its
# own (two senders' faces once stood there)
flat_face() { # flat_face <frame>: the face cell is one tint, round, past half full
	python3 - "$1" $((PANEL_X + 14)) $((CARD_Y + 12)) <<'PY'
import sys
from PIL import Image
px = Image.open(sys.argv[1]).convert('RGB').load()
x0, y0 = int(sys.argv[2]), int(sys.argv[3])
ref = px[x0 + 18, y0 + 31] # under the letters, inside the disc
same = sum(1 for y in range(y0, y0 + 36) for x in range(x0, x0 + 36) if max(abs(a - b) for a, b in zip(px[x, y], ref)) <= 8)
corner = px[x0 + 1, y0 + 1]
sys.exit(0 if max(ref) - min(ref) > 40 and same > 500 and max(abs(a - b) for a, b in zip(corner, ref)) > 30 else 1)
PY
}
nsend -a grp -c im.received -h string:x-notify-conversation-id:grp1 -h string:x-notify-conversation-kind:group \
	-h "string:x-notify-conversation-title:Weekend trip" -h string:x-notify-sender-name:Lan -t 30000 "Weekend trip" "train at 7:40"
hq awesome center >/dev/null
chk "initials: a pictureless group wears its initials' face" shot_until 3 $STATE/initials.png flat_face $STATE/initials.png
hq awesome center >/dev/null
nclear

# ---- hover holds a banner's clock ------------------------------------------
# its 2 s clock outlives the capture and the scan before the pointer lands
# (~1 s); after the leave, the check at ~1.35 s is past what was left of
# it (a resumed clock would be out) and inside a restarted one
nsend -t 2000 "hold me" body; sleep 0.8
capture_nested $STATE/banner-aw.png
chk "banner: it is on screen (the card's pixels)" test "$(banner_px $STATE/banner-aw.png)" -gt 150
printf 'move %s %s\nsleep 3200\n' "$POPX" "$((N_TOP + 20))" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 1.2
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.2
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
nclear

# ---- the glass: the blur's highlights roll off, the drop stays clear --------
# A card carries no tint: over a white window its glass is the blur's
# roll-off of white, 104/255 at highlights 0.46. Without the roll-off it
# reads white; with the card's own drop under it (the drop is cut clear
# of the glass, the fork's "let a shadow clear the shape it drops from")
# about 93. The pointer rests mid-screen: a hovered card lifts.
dsp "hl.dsp.exec_cmd('foot -a glasswhite -o colors-dark.background=ffffff --window-size-pixels=400x200')"; settled glasswhite
dsp "hl.plugin.awesome.maximize()"; settled glasswhite
nsend -t 30000 "glass" body
glass_white() { # the card's right padding, a short column's median
	capture_nested "$STATE/glass-white.png" && python3 - "$STATE/glass-white.png" $((MON_W - N_EDGE - 5)) "$N_TOP" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x, top = int(sys.argv[2]), int(sys.argv[3])
v = sorted(round(sum(px[x, y]) / 3) for y in range(top + 8, top + 20))
open(sys.argv[1][:-4] + '.txt', 'w').write(f'{v}\n')
sys.exit(0 if 96 <= v[len(v) // 2] <= 112 else 1)
PY
}
chk "glass: over white it settles at the roll-off's ceiling, its drop clear of it" until_ok 3 glass_white ||
	echo "glass: over white $(cat "$STATE/glass-white.txt")" >&2
nclear
close_wait "$(class_addr glasswhite)"

# ---- the card menu: hold left ------------------------------------------------
# A hold opens the card's switches at the pointer (hold_menu, mrow above); a
# chat leads with its two mute rows and a separator (these chats offer no
# actions, so no Reply or action rows come first). The switches persist as
# state.tsv rows.
pcidb gatechat Alice alice "are you coming"
chk "menu: a chat banner is up" test "$(bd)" = "banners:1 resident:0"
hold_menu "$HX" "$HY"
chk "menu: the hold fired no click" test "$(st)" = "center:0 live:1 dnd:0"
release_left
chk "menu: releasing on its corner picks nothing" test "$(bd)" = "banners:1 resident:0"
tap esc
click "$HX" "$HY" 272
chk "menu: esc closed it (the next click acts on the card)" test "$(st)" = "center:0 live:0 dnd:0"
pcidb gatechat Alice alice "one"
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 2 1)" 272
chk "menu: Turn Off Banners lowered the app's banner" until_is 3 "banners:0 resident:1" bd
pcidb gatechat Bob bob "ping"
chk "menu: the app's next arrival lands silent" test "$(bd)" = "banners:0 resident:2"
chk "menu: the switch persisted, listed by the app's name" grep -aqx $'notify-app\t1\tgatechat\tgatechat' "$AW_STATE"
# back on from the center: the silenced app's cards fold at the bottom; a
# click spreads them, and the menu opens down-right of a pointer that
# leaves it room
hq awesome center >/dev/null
chk "menu: the silenced app's cards fold into the silent row" until_is 3 "- | silent:2" rows
click "$ROWX" "$ROWY" 272
spread_rows() { [[ "$(rows)" == "- | ~"* ]]; }
chk "menu: a click spreads the fold" until_ok 3 spread_rows
hold_menu "$ROWX" "$ROWY"; release_left
click $((ROWX + 100)) "$(mrow "$ROWY" 2 1)" 272
chk "menu: Turn On from the center drops the row" bash -c "! grep -aq '^notify-app' '$AW_STATE'"
chk "menu: a pick acts on no card" test "$(st)" = "center:1 live:2 dnd:0"
hq awesome center >/dev/null
nclear
# a chat's mute is the chat's alone
pcidb gatechat Alice alice "one"
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
chk "menu: Mute for 1 Hour lowered the chat" until_is 3 "banners:0 resident:1" bd
pcidb gatechat Alice alice "two"
chk "menu: the muted chat's next message lands silent" test "$(bd)" = "banners:0 resident:1"
pcidb gatechat Bob bob "hi"
chk "menu: another chat of the app still banners" test "$(bd)" = "banners:1 resident:1"
chk "menu: the mute persisted, listed by the chat's title" grep -aq $'^notify-mute\t[0-9]*\tAlice\tgatechat\x1falice$' "$AW_STATE"
nclear
pcidb gatechat Alice alice "three"
hq awesome center >/dev/null
chk "menu: the muted chat folds into the silent row" until_is 3 "- | silent:1" rows
click "$ROWX" "$ROWY" 272
chk "menu: ...spread by a click" until_ok 3 spread_rows
hold_menu "$ROWX" "$ROWY"; release_left
click $((ROWX + 100)) "$(mrow "$ROWY" 0 0)" 272
chk "menu: Unmute drops the row" bash -c "! grep -aq '^notify-mute' '$AW_STATE'"
hq awesome center >/dev/null
nclear
# the bell's Silenced Apps lists what the card menus turned off; a click
# turns that one back on
app_rows() { grep -ac '^notify-app' "$AW_STATE"; }
pcidb gatechat Alice alice "four"
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 2 1)" 272
chk "silenced: the app's banners are off" until_is 3 1 app_rows
nclear
bell_open; bell_pick 2
chk "silenced: a click in the bell's list turns them back on" until_is 3 0 app_rows
pcidb gatechat Alice alice "five"
chk "silenced: the app banners again" test "$(bd)" = "banners:1 resident:0"
nclear
# press, hold, slide onto the last row, release: Close All from the app
# the middle banner is a mailer's whichever way the stack runs
nsend -a other -t 30000 "stays" body
nsend -a mailer -t 30000 "report" body
nsend -a mailer -t 30000 "disk" body
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
nsend -t 30000 "ring me" body
chk "bell: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
bell_frame || true
BELLX="$(bell_x "$STATE/bell-probe.png")"
chk "bell: the bell is found in the strip" test "$BELLX" -gt 0
printf 'move %s %s\nsleep 120\nmove %s 13\nsleep 60\npress 272\nsleep 40\nrelease 272\nsleep 80\n' \
    "$(( (BELLX + MON_W / 2) / 2 ))" "$(( MON_H / 2 ))" "$BELLX" | vp
settle
chk "bell: a click opens the shade and absorbs the banner" test "$(st)" = "center:1 live:1 dnd:0"
chk "bell: the absorption parked the banner" test "$(bd)" = "banners:0 resident:1"
hq awesome center >/dev/null
chk "bell: the explicit close re-pops the parked banner" test "$(bd)" = "banners:1 resident:0"
nclear
chk "bell: reset after the bell battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's keyboard nav ----------------------------------------------
# two apps: one app's two cards would stack, and the keys would act on the stack
nsend -a keya -t 60000 -A default=Open "key one" body >/dev/null &
until_is 3 1 hq awesome count
nsend -a keyb -t 60000 "key two" body
hq awesome center >/dev/null
chk "keys: two rows with the shade open" test "$(st)" = "center:1 live:2 dnd:0"
tap down
chk "keys: down only SELECTS — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:2 dnd:0"
tap space
chk "keys: space only folds" test "$(st)" = "center:1 live:2 dnd:0"
tap delete
chk "keys: delete dismisses the selected row" test "$(st)" = "center:1 live:1 dnd:0"
tap enter
chk "keys: enter fires the primary, taking the card and the shade" test "$(st)" = "center:0 live:0 dnd:0"
hq awesome center >/dev/null
chk "keys: a fresh shade for esc to close" test "$(st)" = "center:1 live:0 dnd:0"
tap esc
chk "keys: esc closes the shade" test "$(st)" = "center:0 live:0 dnd:0"
# ---- banners over fullscreen ------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x400')"; settled foot
dsp "hl.dsp.window.fullscreen()"; settled foot
nsend -a q1 -t 30000 over-fullscreen body
expect "fs-banner: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
chk "fs-banner: the banner shows over the fullscreen window" test "$(bd)" = "banners:1 resident:0"
nclear
dsp "hl.dsp.window.close()"; until_is 5 0 pyc "sum(1 for c in cs if c['class'] == 'foot')"

# ---- quiet automatically ------------------------------------------------------
# The bell's Quiet Automatically (When an App Is Fullscreen, While Sharing
# the Screen; both off until turned on): banners and sounds hold, the
# arrival waits in the shade, the banners already up retreat there when
# the quiet begins (a share's once it held a second: a screenshot never
# moves them); critical punches through, over a share without its words. The switches are option rows. Menus first: the bar hides over a
# fullscreen window, and the capture that measures a menu reads any window.
opt_row() { grep -a $'\t'"$1"'$' "$AW_STATE" | cut -f2; }
bell_open; bell_pick 0
chk "quiet: When an App Is Fullscreen turns on, remembered" until_is 3 1 opt_row quiet-fullscreen
dsp "hl.dsp.exec_cmd('foot -a quietfs --window-size-pixels=600x400')"; wait_class quietfs
nsend -a q2 -t 30000 before body
chk "quiet: a banner up before the fullscreen" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.window.fullscreen()"
chk "quiet: going fullscreen sends it to the shade" until_is 3 "banners:0 resident:1" bd
nsend -a q2 -t 30000 held body
chk "quiet: over a fullscreen focus an arrival waits in the shade" test "$(bd)" = "banners:0 resident:2"
chk "quiet: the probe agrees" test "$(hq awesome quiet)" = "now:1 hushed:0"
nsend -a q2 -u critical -t 30000 urgent body
chk "quiet: critical punches through" test "$(bd)" = "banners:1 resident:2"
nclear
dsp "hl.dsp.window.fullscreen()"; settled quietfs
nsend -a q2 -t 30000 free body
chk "quiet: out of fullscreen it banners again" test "$(bd)" = "banners:1 resident:0"
nclear
dsp "hl.dsp.window.close()"; until_is 5 0 pyc "sum(1 for c in cs if c['class'] == 'quietfs')"
bell_open; bell_pick 0
chk "quiet: turned off, the row goes" until_is 3 "" opt_row quiet-fullscreen
# shares: the fixture copies the nested's output over wlr-screencopy (the
# portal's protocol); the nested renders on damage, so the pointer jitters
# until a frame is copied
share_start() { # share_start <log> [sharehold flags]: SHARE_PID holds it
	local log=$1 _
	shift
	WAYLAND_DISPLAY="$WL" "$REPO/devtools/sharehold" "$@" >"$log" 2>&1 & SHARE_PID=$!
	for _ in $(seq 1 25); do
		grep -q READY "$log" && return 0
		printf 'rel 2 0\nsleep 40\nrel -2 0\nsleep 40\n' | vp; sleep 0.1
	done
	return 1
}
share_stop() { kill "$SHARE_PID" 2>/dev/null; wait "$SHARE_PID" 2>/dev/null; SHARE_PID=""; }
bell_open; bell_pick 1
chk "quiet: While Sharing the Screen turns on, remembered" until_is 3 1 opt_row quiet-sharing
nsend -a q3 -t 30000 before body
chk "quiet: a banner up before the share" test "$(bd)" = "banners:1 resident:0"
# a screenshot: one frame, its client still connected (its session stays)
chk "quiet: a one-frame screenshot is copied" share_start "$STATE/shot.log" -n 1
sleep 1.6
chk "quiet: the screenshot leaves the banner up" test "$(bd)" = "banners:1 resident:0"
chk "quiet: ...and is no share once copied" until_is 3 "now:0 hushed:0" hq awesome quiet
share_stop
# a screencast: copies with damage, so a still screen keeps a frame waiting
chk "quiet: a screencast is copied" share_start "$STATE/share.log" -d
chk "quiet: the share sends the banner to the shade" until_is 4 "banners:0 resident:1" bd
sleep 1.5
chk "quiet: a still screen stays shared" test "$(hq awesome quiet)" = "now:1 hushed:1"
nsend -a q3 -t 30000 held body
chk "quiet: while shared an arrival waits in the shade" test "$(bd)" = "banners:0 resident:2"
nsend -a q3 -u critical -t 30000 urgent secret
chk "quiet: critical punches through the share" test "$(bd)" = "banners:1 resident:2"
chk "quiet: ...holding its words back" test "$(hq awesome quiet)" = "now:1 hushed:1"
hq awesome center >/dev/null
chk "quiet: the open center says why arrivals hold" until_is 3 "quiet:while you share the screen" rows_status
hq awesome center >/dev/null
share_stop
chk "quiet: the share's end gives the words back" until_is 4 "now:0 hushed:0" hq awesome quiet
nclear
bell_open; bell_pick 1
chk "quiet: sharing off, the row goes" until_is 3 "" opt_row quiet-sharing

# ---- bus: closing an unknown ID is an error --------------------------------
nsend -a unk -t 30000 unknown "close probe body"
chk "bus-close: a live card for the probe" test "$(bd)" = "banners:1 resident:0"
chk "bus-close: unknown id is an error" bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 4294967000 2>&1 | grep -q 'Call failed: Unknown notification ID'"
chk "bus-close: the failed close touched nothing" test "$(st)" = "center:0 live:1 dnd:0"
nclear

# ---- repop ---------------------------------------------------------------------
nsend -t 30000 "repop me" body
chk "repop: a live banner before the open" test "$(bd)" = "banners:1 resident:0"
hq awesome center >/dev/null
chk "repop: the open absorbed it" test "$(bd)" = "banners:0 resident:1"
outside_click
chk "repop: the outside-click close popped it back" test "$(bd)" = "banners:1 resident:0"
nclear

# ---- DND -----------------------------------------------------------------------
dsp "hl.plugin.awesome.suspend()"
chk "DND arms" hq_matches 'dnd:1' awesome state
chk "DND: remembered while it holds (until turned off)" until_is 3 "-1" dndrow
for m in one two three four; do nsend -a q $m body; done
chk "DND: four same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.awesome.suspend()"
chk "DND resume: three popped, the fourth resumed resident (the per-app cap)" test "$(bd)" = "banners:3 resident:1"
chk "DND resume: dnd off, every card kept" hq_matches '^center:0 live:4 dnd:0$' awesome state
chk "DND: off keeps no row" until_is 3 "" dndrow
nclear
# while DND holds, the center's status line says so, and how many wait
dsp "hl.plugin.awesome.suspend()"
nsend -a q waiting body
hq awesome center >/dev/null
chk "status: the center says why arrivals hold, and how many wait" until_is 3 "dnd:until turned off · 1 waiting |" rows
status_drawn() { (($(lit_px "$1" $((PANEL_X + 8)) $((PANEL_X + 28)) $((CARD_Y + 8)) $((CARD_Y + 28))) > 250)); }
chk "status: its line stands under the heading" shot_until 3 $STATE/status.png status_drawn $STATE/status.png
nsend -a q waiting2 body
chk "status: an arrival held meanwhile counts at once" until_is 3 "dnd:until turned off · 2 waiting |" rows
hq awesome center >/dev/null
dsp "hl.plugin.awesome.suspend()"
chk "status: DND off" hq_matches 'dnd:0' awesome state
nclear

# ---- DND for a while ------------------------------------------------------------
# suspend(minutes) holds DND to a wall-clock end, which the row keeps; the
# end comes by itself and resumes the queue.
dnd_end_ahead() { local v; v="$(dndrow)"; [[ "$v" =~ ^[0-9]+$ ]] && ((v > $(date +%s))); }
dsp "hl.plugin.awesome.suspend(0.05)"
chk "DND timed: arms" test "$(st)" = "center:0 live:0 dnd:1"
chk "DND timed: remembered with its end" dnd_end_ahead
nsend -a q held body
chk "DND timed: an arrival waits" test "$(bd)" = "banners:0 resident:0"
chk "DND timed: the end resumes it by itself" until_is 10 "banners:1 resident:0" bd
chk "DND timed: off, its row gone" until_is 3 "" dndrow
chk "DND timed: dnd off" test "$(st)" = "center:0 live:1 dnd:0"
nclear

# ---- the bell's options: a right click ----------------------------------------
# One flat menu at the bell (shell/bell.cpp): Quiet Automatically's header
# and its two switches, a separator, Silenced Apps' header and its rows —
# "None" here. Do Not Disturb is the control center's alone.
bell_open
chk "bell: a right click opens its options" test $((BRX1 - BRX0)) -ge 190
chk "bell: the right click left the center shut" test "$(st)" = "center:0 live:0 dnd:0"
# 2 x 5 + 2 headers x 22 + 3 rows x 28 + a separator 9 = 147, + the rims
chk "bell: the options are one flat panel (no cascade)" test $((BRY1 - BRY0 + 1)) -ge 145 -a $((BRY1 - BRY0 + 1)) -le 153
click 640 400 272

# ---- DND holds a merge onto an existing card ------------------------------------
# A conversation's first message retreats to the shade; with DND armed the
# next message merges onto that card — it must not pop the banner DND is
# holding back.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i dndchat 0 "" Alice one 0 3 desktop-entry s dndchat category s im.received conversation-id s cid-dnd 600 >/dev/null 2>&1
until_is 3 "banners:0 resident:1" bd
dsp "hl.plugin.awesome.suspend()"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i dndchat 0 "" Alice two 0 3 desktop-entry s dndchat category s im.received conversation-id s cid-dnd 30000 >/dev/null 2>&1
chk "DND: a merge onto a shaded card stays in the shade" test "$(bd)" = "banners:0 resident:1"
chk "DND: the merge landed on the one card" test "$(st)" = "center:0 live:1 dnd:1"
dsp "hl.plugin.awesome.suspend()"
nclear

# ---- hostile hints ---------------------------------------------------------------
nsend -h int:category:5 "badcat" body
nsend -h string:category:im.received "convo" body
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
nclear

# ---- inline reply -----------------------------------------------------------------
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
bus_watch "$REPLIED" "type='signal',member='NotificationReplied'"
conv_reply_notify Telegram "Alice" "gone already"
hq awesome center >/dev/null
tap down
tap tab
tap esc
chk "reply: a card whose sender left the bus opens no field (esc closes the shade)" test "$(st)" = "center:0 live:1 dnd:0"
nclear
chk "reply: PyGObject for a sender that stays on the bus" python3 -c 'from gi.repository import Gio'
conv_reply_sender Telegram "Alice" "are you around?"
sent_wait
hq awesome center >/dev/null
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
chk "reply: enter sent it and the card went" test "$(st)" = "center:1 live:0 dnd:0"
chk "reply: NotificationReplied carried the typed text" until_ok 3 grep -q 'STRING "hi"' "$REPLIED"
kill "$REPLY_SENDER" 2>/dev/null; wait "$REPLY_SENDER" 2>/dev/null
hq awesome center >/dev/null
nclear

# Reply from a banner's menu (its first row): the field opens in the banner,
# which holds its clock and takes the typing; Enter sends from there
REPLIED2="$STATE/replied-banner.log"
rm -f "$REPLIED2"
bus_watch "$REPLIED2" "type='signal',member='NotificationReplied'"
conv_reply_sender Telegram "Alice" "are you around?"
sent_wait
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
tap 35
tap 23
chk "reply: a banner's field takes the typing and acts on nothing" test "$(st)" = "center:0 live:1 dnd:0"
tap enter
chk "reply: enter sent it from the banner and the card went" until_is 3 "center:0 live:0 dnd:0" st
chk "reply: NotificationReplied carried the banner's text" until_ok 3 grep -q 'STRING "hi"' "$REPLIED2"
kill "$REPLY_SENDER" 2>/dev/null; wait "$REPLY_SENDER" 2>/dev/null
nclear

# ---- a Telegram group: one card, a notification per message ---------------------
# Telegram Desktop posts each message on its own and keeps a record per
# message: a signal goes to the FIRST record holding its id, and a group's
# reply is sent as a reply to that record's message. One shared id made the
# card's Reply quote the chat's oldest message, and the banner, which kept
# the joined text's head, never showed the newest. Each merged message now
# answers for an id of its own, the card acts for the newest, shows what
# fits from the newest end, and lets the sender close one message alone.
TGIDS=$STATE/tg-ids TGSIG=$STATE/tg-signals.log
rm -f "$TGIDS" "$TGSIG"
bus_watch "$TGSIG" "type='signal',interface='org.freedesktop.Notifications'"
tgchat "$TGIDS" "Rice Club|Alice|anyone tried the new blur passes?" "Rice Club|Bob|yeah, size 5 with 2 passes looks clean" \
	"Rice Club|Alice|screenshot pls" "Rice Club|Minh|Photo" \
	"Rice Club|Bob|also turn off new_optimizations if your wallpaper is animated, otherwise the blur under the bar lags a frame behind and looks like it breathes" \
	"Rice Club|Charlotte|ok" "Rice Club|Bob|reacted to your message" "Rice Club|Alice|nice, thanks Bob!"
until_is 5 8 bash -c "wc -l <'$TGIDS'"
chk "tg: eight messages, one card" test "$(st)" = "center:0 live:1 dnd:0"
chk "tg: every message answers for an id of its own" test "$(sort -u "$TGIDS" | wc -l)" -eq 8
# a card with a picture waits for its pixels, and "shown" is "" until a
# frame painted it: these read what is drawn once it is
shown_ends() { local s; s="$(hq awesome shown)"; [[ "${s##*$'\n'}" == "$1" ]]; }
chk "tg: the banner ends on the newest message" until_ok 3 shown_ends "<b>Alice</b>: nice, thanks Bob!"
SHOWN="$(hq awesome shown)"
# the stored log holds seven; the banner's lines hold fewer of them
chk "tg: the window is fitted, the stored log's oldest left out" test "${SHOWN#*yeah, size 5}" = "$SHOWN"
hold_menu "$HX" "$HY"; release_left
click $((HX - 100)) "$(mrow "$HY" 0 0)" 272
tap 35
tap 23
tap enter
tg_replied() { [[ "$(sigs "$TGSIG" | awk '$1 == "NotificationReplied" { print $2; exit }')" == "$(tail -1 "$TGIDS")" ]]; }
chk "tg: Reply quoted the newest message" until_ok 3 tg_replied
chk "tg: the card's close closed every message's notification" test "$(sigs "$TGSIG" | awk '$1 == "NotificationClosed" && $3 == 2 { print $2 }' | sort -u | wc -l)" -eq 8
nfyact_done
nclear
tgchat "$TGIDS" "Trip|Linh|bus leaves at 7:30" "Trip|Quang|packing list" "Trip|Linh|see you there"
until_is 5 3 bash -c "wc -l <'$TGIDS'"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u "$(tail -1 "$TGIDS")"
chk "tg: the sender closing one message keeps its chat" test "$(st)" = "center:0 live:1 dnd:0"
chk "tg: and takes that message's line alone" until_ok 3 shown_ends "<b>Quang</b>: packing list"
for i in 1 2; do
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u "$(sed -n ${i}p "$TGIDS")"
done
chk "tg: the chat's last message takes its card" until_is 3 "center:0 live:0 dnd:0" st
nfyact_done
tgchat "$TGIDS" "Chat A|Xu|one" "Chat B|Yo|two" "Chat A|Xu|three"
until_is 5 3 bash -c "wc -l <'$TGIDS'"
chk "tg: a chat's news leads the column" until_is 3 "<b>Xu</b>: three" hq awesome topline
nfyact_done
nclear
tgchat "$TGIDS" "Telegram Desktop||You have a new message" "Telegram Desktop||You have a new message" "Telegram Desktop||You have a new message"
until_is 5 3 bash -c "wc -l <'$TGIDS'"
chk "tg: a hidden preview's repeats show once" until_is 3 "You have a new message" hq awesome shown
nfyact_done
nclear
# a client that keeps one id for its chat replaces it with every message
# (conversation-id and message-id hints): each Notify answers with that
# id, as the spec has it, and closing it closes the whole chat
onechat() { # onechat <replaces> <message-id> <text>: prints the id answered
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i onechat "$1" "" Room "$3" 0 3 category s im.received conversation-id s cid-one message-id s "$2" 30000 2>/dev/null | awk '{ print $2 }'
}
R1="$(onechat 0 m1 first)"
R2="$(onechat "$R1" m2 second)"
R3="$(onechat "$R1" m3 third)"
chk "chat: a replace answers with its replaces_id" test "$R2:$R3" = "$R1:$R1"
chk "chat: its messages join one card" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u "$R1"
chk "chat: closing the one id closes the whole chat" until_is 3 "center:0 live:0 dnd:0" st
nclear
# a forum's picture comes shaped (a rounded square, its corners clear) and
# keeps its shape; an opaque square is cut round. The point 4 px into the
# face's box lies inside the rounded square (inked from 3 px) and outside
# the circle (whose anti-aliased edge is 5 px in).
python3 - "$STATE/tg-forum.raw" "$STATE/tg-square.raw" <<'PY'
import sys
W, R, INK = 64, 19, bytes((230, 40, 200, 255))
def px(x, y):
    cx, cy = min(max(x + 0.5, R), W - R), min(max(y + 0.5, R), W - R)
    return INK if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= R * R else bytes(4)
open(sys.argv[1], "wb").write(b"".join(px(x, y) for y in range(W) for x in range(W)))
open(sys.argv[2], "wb").write(INK * (W * W))
PY
face_corner() { # face_corner <frame>: 1 when the face's ink covers its box's corner
	python3 - "$1" "$((PANEL_X + 14 + 4))" "$((N_TOP + 12 + 4))" <<'PY'
import sys
from PIL import Image
r, g, b = Image.open(sys.argv[1]).convert("RGB").getpixel((int(sys.argv[2]), int(sys.argv[3])))
print(1 if r > 150 and b > 120 and g < 110 else 0)
PY
}
tgchat "$TGIDS" "Releases (Dev)|Mira|v1 is tagged" "@$STATE/tg-forum.raw"
until_is 5 1 bash -c "wc -l <'$TGIDS'"
capture_nested $STATE/tg-forum.png
chk "tg: a shaped chat picture keeps its shape" test "$(face_corner $STATE/tg-forum.png)" = 1
nfyact_done
nclear
tgchat "$TGIDS" "Plain Group|Mira|hello" "@$STATE/tg-square.raw"
until_is 5 1 bash -c "wc -l <'$TGIDS'"
capture_nested $STATE/tg-square.png
chk "tg: an opaque square chat picture is cut round" test "$(face_corner $STATE/tg-square.png)" = 0
nfyact_done
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
until_is 3 4 bash -c "wc -l < '$STATE/hang-sound.pid'"
sleep 0.5 # a fifth spawn would land in this beat
chk "sound: the spawn cap holds" test "$(wc -l < "$STATE/hang-sound.pid" 2>/dev/null || echo 0)" -eq 4
chk "sound: the capped spawns still carried every card" test "$(st)" = "center:0 live:20 dnd:0"
# every SOUND slot is held: the volume keys still have theirs
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"
chk "sound: a sound flood leaves the volume keys their own slots" until_ok 3 grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
xargs -r kill 2>/dev/null <"$STATE/hang-sound.pid"
rm -f "$STATE/hang-sound" "${STATE}/hang-sound.pid"
nclear
chk "sound: the flood cleared" test "$(st)" = "center:0 live:0 dnd:0"

# ---- a transient card is feedback, never a shade row ------------------------
# A screenshot tool's preview: on screen while it lasts, never history. The
# shade opening over it ends it rather than parking it as a row.
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "shot" 0 "" "Screenshot" "copied" 0 1 transient b true 30000 >/dev/null 2>&1
chk "transient: the card is live" test "$(st)" = "center:0 live:1 dnd:0"
chk "transient: no banner or resident on the badge" test "$(bd)" = "banners:0 resident:0"
hq awesome center >/dev/null
chk "transient: opening the shade ends the card" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null
chk "transient: closing the shade brings nothing back" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the module leaves the plugin exactly as the preflight found it --------
chk "notifications: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
chk "notifications: no banners or residents" test "$(bd)" = "banners:0 resident:0"
