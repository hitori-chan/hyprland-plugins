#!/usr/bin/env bash
# awesome/gate/lib.sh — the shared probe helpers for the batteries. The
# control verbs are `hyprctl awesome <verb>`; the bell is driven by a REAL
# pointer over the bell glyph — hover (a no-op) and click are widget
# events, there is no bus verb for either.
# Pure definitions only — no side effects, no battery code.

# ---- geometry (derived from notify/ui.hpp) ------------------------------
N_EDGE=10
N_W=348
CENTER_W=360
PANEL_X=$((MON_W - N_EDGE - CENTER_W))
ROWX=$((PANEL_X + 10 + 80))
ROWY=64
CHVX=$((MON_W - 10 - 10 - 12 - 12))
POPX=$((MON_W - N_EDGE - N_W / 2))

# ---- input -------------------------------------------------------------------
click() {
	printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
		vp
	sleep 0.35
}
tap() {
	printf 'tap %s\nsleep 250\n' "$1" | vk
	sleep 0.6
}
wheel() {
	printf 'move %s %s\nsleep 60\nscroll 0 %s\nsleep 200\n' "$ROWX" "$ROWY" "$1" |
		vp
	sleep 0.3
}
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }

# ---- the bell, pointer-driven ------------------------------------------------
# The monolith's bell is a no-op on hover and toggles the shade on click;
# both are REAL pointer events over the glyph. The glyph is the leftmost
# bright run of the bar's right cluster (tray items sit further left and are
# magenta; the battery and the clock sit to its right), so it is found by
# pixel, not by a fixed inset: the clock and the battery text change width
# under the gate and would drift a constant.
bell_x() { # bell_x <frame> — the bell glyph's center x (0 when absent)
	# The bell is the leftmost element of the right slot (then badge, battery,
	# clock). The badge is glued to the glyph's right edge (gap <= 2), so the
	# first bright run in the right half is GLYPH+BADGE merged — its center
	# lands on the battery, 5px past the bell's 32px hitbox. The glyph is 16px
	# wide (shell/bell.cpp GLYPH) and starts at the run's first column, so the
	# glyph center is first + 8, always inside the hitbox.
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
w = int(sys.argv[2])
x0 = w // 2
for x in range(x0, w):
    if any(min(px[x, y][:3]) > 120 for y in range(4, 24)):
        print(x + 8)
        sys.exit()
print(0)
PY
}
bell_frame() { capture_nested "$STATE/bell-probe.png"; }

# ---- bounded waits ----------------------------------------------------------------
# until_is <seconds> <want> <cmd...>: poll until the command prints `want` —
# an asynchronous effect landing (an arrival, a sweep) — false on timeout.
# For final states only: a "nothing happened" check keeps a fixed settle.
until_is() {
	local n=$(($1 * 10)) want=$2
	shift 2
	while ((n-- > 0)); do
		[[ "$("$@" 2>/dev/null)" == "$want" ]] && return 0
		sleep 0.1
	done
	return 1
}
# sweep the model and wait until it is empty (the shade may stay open)
nclear() {
	hq awesome clear >/dev/null 2>&1
	local n=30
	while ((n-- > 0)) && [[ "$(st)" != *" live:0 "* ]]; do sleep 0.1; done
	sleep 0.2
}

# ---- model strings -------------------------------------------------------------
st() { hq awesome state; }
bd() { hq awesome badge; }
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }

# ---- sends -----------------------------------------------------------------------
nfyact() {
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
pcidb() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$4" 0 3 desktop-entry s "$1" category s im.received conversation-id s "$3" 30000 >/dev/null 2>&1
}
tdgrp() {
	local body
	body=$(printf '<b>%s</b>\n%s' "$2" "$3")
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "Telegram" 0 "" "$1" "$body" 0 3 suppress-sound b true category s im.received desktop-entry s telegram-desktop 30000 >/dev/null 2>&1
}
conv_reply_notify() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 2 inline-reply Reply 1 category s im.received 60000 >/dev/null 2>&1
}
# The same card from a sender that stays on the bus, as a chat app does: a
# card whose sender left (busctl's one-shot call above) is ownerless and
# opens no reply field. Sets REPLY_SENDER; the caller kills it.
conv_reply_sender() {
	DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 python3 - "$1" "$2" "$3" <<'PY' &
import sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
              GLib.Variant("(susssasa{sv}i)", (sys.argv[1], 0, "", sys.argv[2], sys.argv[3], ["inline-reply", "Reply"],
                                               {"category": GLib.Variant("s", "im.received")}, 60000)),
              None, Gio.DBusCallFlags.NONE, -1, None)
GLib.MainLoop().run()
PY
	REPLY_SENDER=$!
}

# ---- pixels ------------------------------------------------------------------------
panel_bottom() {
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.load()
x0 = int(sys.argv[2]) - 392 + 2
x1 = int(sys.argv[2]) - 12
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

# ---- the strip (shell) ----------------------------------------------------------------
# The bar's own pixel probes. The strip is the top band (y 0..barH-1); the
# background is <=5 by design, so a "bright" pixel is strip content.
bar_bright() { # bar_bright <frame> — bright-pixel count in the whole strip band
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
w, h = im.size
n = 0
for y in range(0, 26):
    for x in range(0, w, 2):
        if min(px[x, y][:3]) > 120:
            n += 1
print(n)
PY
}
banner_px() { # banner_px <frame> — bright count in the single-banner card area
	# the card is glass (dark) with bright text/icon: the content spans the
	# whole card height (title + body + icon + the optional progress bar), so
	# the band runs the card's full extent and the threshold counts glyphs.
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
w = int(sys.argv[2])
x0, x1 = w - 10 - 348 + 10, w - 10 - 10
n = 0
for y in range(55, 185):
    for x in range(x0, x1, 2):
        if min(px[x, y][:3]) > 60:
            n += 1
print(n)
PY
}
strip_band() { # strip_band <frame> <x0> <x1> <minchan> — bright count in a band
	python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0, x1, mc = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
n = 0
for y in range(0, 26):
    for x in range(x0, min(x1, im.size[0]), 2):
        if min(px[x, y][:3]) > mc:
            n += 1
print(n)
PY
}
