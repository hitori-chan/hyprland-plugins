#!/usr/bin/env bash
# awesome/gate/lib.sh — the shared probe helpers for the batteries. The
# control verbs are `hyprctl awesome <verb>`; the bell is driven by a REAL
# pointer over the bell glyph — hover (a no-op) and click are widget
# events, there is no bus verb for either.
# Pure definitions only — no side effects, no battery code.

# ---- geometry (derived from notify/ui.hpp, notify/center.cpp) ------------
# the column's right inset, a card's width, the center's top; the first
# center card starts under its 28 px header + 4 px gap
N_EDGE=12
N_W=356
N_TOP=40
PANEL_X=$((MON_W - N_EDGE - N_W))
CARD_Y=$((N_TOP + 28 + 4))
ROWX=$((PANEL_X + 90))
ROWY=$((CARD_Y + 24))
POPX=$((MON_W - N_EDGE - N_W / 2))
BAR_H=28
# shell/taglist.cpp: 26 px cells, 2 apart, from the bar's 8 px inset
tag_cx() { echo $((8 + ($1 - 1) * 28 + 13)); }

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
# both are REAL pointer events over the widget. The right slot is [tray]
# [battery][bell][clock], 20 px apart (shell/strip.cpp RIGHT_GAP): the
# clock is the last run of white ink from the screen's right edge, the bell
# the one before it — found by pixel, as the clock, the battery and the tray
# change width under the gate. White ink is every channel past 150, so
# neither the green dot nor the accent counts — and so with the center
# CLOSED only: an open center lights the bell in the accent.
bell_x() { # bell_x <frame> — the bell glyph's center x (0 when not found)
	python3 - "$1" "$MON_W" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load(); w = int(sys.argv[2])
# the clock's word spaces leave ~8 px of no ink; the gaps between widgets 20+
runs = []
for x in range(w - 1, w // 2, -1):
    if any(min(px[x, y]) > 150 for y in range(6, 23)):
        if runs and runs[-1][0] - x < 12:
            runs[-1][0] = x
        else:
            runs.append([x, x])
print((runs[1][0] + runs[1][1]) // 2 if len(runs) >= 2 else 0)
PY
}
# the bell's dot: col_dot green (0xff32d74b) in the bell's cell; the battery
# sits a gap and more to its left (a charging fill is the same green)
dot_px() { # dot_px <frame> — green pixels on the bell (0 when no dot)
	python3 - "$1" "$(bell_x "$1")" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load(); bx = int(sys.argv[2])
if bx <= 0:
    print(0); raise SystemExit
n = 0
for x in range(max(0, bx - 10), min(im.width, bx + 12)):
    for y in range(0, 28):
        r, g, b = px[x, y]
        n += g > 170 and r < 120 and b < 140
print(n)
PY
}
# strip_drawn <frame> — the strip finished its warm: the clock and the bell
# draw at its right end (bell_x finds the bell past the clock) and the viewed
# tag draws in the accent at its left. Structure, not a brightness count: a
# count moves with the theme, the face and the hardware, and a mid-warm frame
# lacks whole glyphs.
strip_drawn() { [[ "$(bell_x "$1")" -gt 0 && "$(accent_px "$1" 8 34)" -gt 6 ]]; } # 一 is one stroke: ~10 px
bell_frame() { capture_nested "$STATE/bell-probe.png"; }

# ---- bounded waits ----------------------------------------------------------------
# wait_class <class>: a window of that class (its app id) is mapped
wait_class() { for _ in $(seq 1 40); do [[ "$(pyc "any(c['class']=='$1' for c in cs)")" = 1 ]] && return 0; sleep 0.25; done; return 1; }
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
# A card with actions from a sender that STAYS on the bus, as an app does: a
# departed sender's card loses its actions (busctl's one-shot call would
# leave before the click). Hints are name/type/value triples (s b i u y).
# The sender lives until nfyact_done or its timeout.
NFYACT_SENDERS=()
nfyact() {
	DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 python3 - "$@" <<'PY' &
import sys
from gi.repository import Gio, GLib
a = sys.argv[1:]
app, summary, a = a[0], a[1], a[2:]
n = int(a[0]); acts, a = a[1:1 + n], a[1 + n:]
n = int(a[0]); a = a[1:]
def hint(t, v):
    return GLib.Variant(t, v == "true" if t == "b" else int(v) if t in "iuy" else v)
hints = {a[3 * i]: hint(a[3 * i + 1], a[3 * i + 2]) for i in range(n)}
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
              GLib.Variant("(susssasa{sv}i)", (app, 0, "", summary, "body", acts, hints, 30000)),
              None, Gio.DBusCallFlags.NONE, -1, None)
GLib.MainLoop().run()
PY
	NFYACT_SENDERS+=($!)
}
nfyact_done() {
	((${#NFYACT_SENDERS[@]})) || return 0
	kill "${NFYACT_SENDERS[@]}" 2>/dev/null
	wait "${NFYACT_SENDERS[@]}" 2>/dev/null
	NFYACT_SENDERS=()
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
panel_bottom() { # panel_bottom <frame> — the center column's last lit row (0 when empty)
	# from inside the first card down: the heading above it is text, not a
	# fill, and the cards stand 8 px apart (a gap past 12 rows ends it)
	python3 - "$1" "$MON_W" "$CARD_Y" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.load()
x0 = int(sys.argv[2]) - 12 - 356 + 2
x1 = int(sys.argv[2]) - 12 - 2
def filled(y):
    for x in range(x0, x1, 3):
        if max(px[x, y]) > 5:
            return True
    return False
H = im.size[1]
top = next((y for y in range(int(sys.argv[3]) + 2, H) if filled(y)), None)
if top is None:
    print(0)
    raise SystemExit
bot = top
for y in range(top, H):
    if filled(y):
        bot = y
    elif all(not filled(yy) for yy in range(y, min(y + 12, H))):
        break
print(bot)
PY
}

# ---- the strip (shell) ----------------------------------------------------------------
# The bar's own pixel probes. The strip is the top band (y 0..BAR_H-1); the
# nested's background is black, so a "bright" pixel is strip content.
bar_bright() { # bar_bright <frame> — bright-pixel count in the whole strip band
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
w, h = im.size
n = 0
for y in range(0, 28):
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
x0, x1 = w - 12 - 356 + 14, w - 12 - 14
n = 0
for y in range(40, 170):
    for x in range(x0, x1, 2):
        if min(px[x, y][:3]) > 60:
            n += 1
print(n)
PY
}
task_band() { # task_band <frame> — lit task-title pixels (focused accent or INK)
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
n = 0
for y in range(0, 28):
    for x in range(280, min(900, im.size[0]), 2):
        if max(px[x, y][:3]) > 100:
            n += 1
print(n)
PY
}
accent_px() { # accent_px <frame> <x0> <x1> [y0 y1] — accent pixels in a box (default: the strip)
	python3 - "$1" "$2" "$3" "${4:-0}" "${5:-$BAR_H}" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
n = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        r, g, b = px[x, y]
        if b > 200 and g > 150 and r < 80:
            n += 1
print(n)
PY
}
lit_px() { # lit_px <frame> <x0> <x1> <y0> <y1> — ink pixels (any channel past 60) in a box
	python3 - "$1" "$2" "$3" "$4" "$5" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
print(sum(1 for y in range(y0, y1) for x in range(x0, x1) if max(px[x, y]) > 60))
PY
}
