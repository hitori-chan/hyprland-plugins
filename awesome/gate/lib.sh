#!/usr/bin/env bash
# awesome/gate/lib.sh — the shared probe helpers for the batteries. The
# control verbs are `hyprctl awesome <verb>`; the bell's hover-peek is
# driven by a REAL pointer over the bell glyph — the hover is a widget
# event, there is no bus verb.
# Pure definitions only — no side effects, no battery code.

# ---- geometry (derived from notify/ui.hpp) ------------------------------
N_EDGE=10
N_W=348
CENTER_W=360
N_OFFSET=34
N_MARGIN=6
ROW_H=59
ISL_X=$((MON_W - N_EDGE - N_W))
PANEL_X=$((MON_W - N_EDGE - CENTER_W))
ROWX=$((PANEL_X + 10 + 80))
ROWY=64
CHVX=$((MON_W - 10 - 10 - 12 - 12))
ROWMID_X=$((MON_W - 200))
POPX=$((MON_W - N_EDGE - N_W / 2))
DND_X=$((PANEL_X + 10 + 17))

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
swipe() {
	printf 'move %s %s\nsleep 60\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 200\n' "$1" "$2" "$3" "$3" "$3" |
		vp
	sleep 0.9
}
wheel() {
	printf 'move %s %s\nsleep 60\nscroll 0 %s\nsleep 200\n' "$ROWX" "$ROWY" "$1" |
		vp
	sleep 0.3
}
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }

# ---- the bell's hover-peek, pointer-driven ----------------------------------
# The monolith's bell opens the shade unpinned on a REAL hover and closes it
# on leave (the 400ms grace, self-healed by the shell's 200ms tick). The bell
# glyph is the leftmost bright run of the bar's right cluster (tray items sit
# further left and are magenta; the battery and the clock sit to its right),
# so it is found by pixel, not by a fixed inset: the clock and the battery
# text change width under the gate and would drift a constant.
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
bell_hover() { # bell_hover <true|false> — the real pointer over/off the bell
	# Two moves, not one: the compositor dedups a motion whose floored
	# position equals the last DELIVERED one, and that remembered position is
	# frozen at the last non-cancelled point (every move over the bar is
	# cancelled, so it never advances). A first move to a fresh off-bar
	# position guarantees the final move to the bell is a distinct event.
	local on=$1 bx
	bell_frame || return 1
	bx="$(bell_x "$STATE/bell-probe.png")"
	[[ "$bx" -gt 0 ]] || return 1
	if [[ "$on" == true ]]; then
		printf 'move %s %s\nsleep 120\nmove %s 13\nsleep 600\n' \
		"$(( (bx + MON_W / 2) / 2 ))" "$(( MON_H / 2 ))" "$bx" | vp
	else
		printf 'move %s %s\nsleep 900\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
	fi
}

# ---- model strings -------------------------------------------------------------
st() { hq awesome state; }
bd() { hq awesome badge; }
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }

# ---- sends -----------------------------------------------------------------------
nfy() {
	local app=$1 sum=$2 body=${3-body}
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" "$body" 0 0 30000 >/dev/null 2>&1
}
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
psend() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" category s "$3" 30000 >/dev/null 2>&1
}
pcid() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 3 desktop-entry s "$1" category s im.received conversation-id s "$3" 30000 >/dev/null 2>&1
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
pcrit() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" urgency y 2 30000 >/dev/null 2>&1
}
ptran() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 3 desktop-entry s "$1" category s im.received transient b true 30000 >/dev/null 2>&1
}
conv_notify() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 0 2 desktop-entry s "$1" category s im.received 30000 >/dev/null 2>&1
}
conv_reply_notify() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 2 inline-reply Reply 1 category s im.received 60000 >/dev/null 2>&1
}
nfyid() {
	local app=$1 sum=$2
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' "$app" 0 "" "$sum" body 0 0 30000 2>/dev/null |
		awk 'NR==1{print $2}'
}
closeid() {
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		CloseNotification u "$1" >/dev/null 2>&1
}

# ---- center ---------------------------------------------------------------------------
center_off() { [[ "$(st)" == center:1* ]] && { hq awesome center >/dev/null; sleep 0.4; }; }
center_on() { [[ "$(st)" != center:1* ]] && { hq awesome center >/dev/null; sleep 0.5; }; }

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
settle_frame() {
	local out=$1 want=$2 pb
	for _ in $(seq 1 12); do
		capture_nested "$out" || return 1
		pb="$(panel_bottom "$out")"
		[[ $((pb - 26)) -eq "$want" ]] && return 0
		sleep 0.12
	done
	return 1
}
expect_panel() {
	if settle_frame "$2" "$3"; then
		ok "$1"
	else
		local pb; pb="$(panel_bottom "$2")"
		command cp -f "$2" "/tmp/capfail-$(basename "$2")" 2>/dev/null
		bad "$1 (want panel h $3, got $((pb - 26)))"
	fi
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
