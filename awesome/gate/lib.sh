#!/usr/bin/env bash
# awesome/gate/lib.sh — the shared probe helpers for the batteries. The
# control verbs are `hyprctl awesome <verb>`; the bell is driven by a REAL
# pointer over the bell glyph — hover (a no-op) and click are widget
# events, there is no bus verb for either.
# Pure definitions only — no side effects, no battery code.

# ---- geometry (derived from notify/ui.hpp, center.cpp, controls.cpp) ------
# the column's right inset, a card's width, its top; the center opens on
# its 28 px heading row, then 8 px
N_EDGE=12
N_W=356
N_TOP=40
PANEL_X=$((MON_W - N_EDGE - N_W))
HEAD_Y=$N_TOP
CARD_Y=$((HEAD_Y + 28 + 8))
# The control center, in the same column (notify/controls.cpp): a 64 px
# module per level, 12 apart (0 display, 1 keyboard, 2 volume: all three in
# the gate), its section 10 px down (216 tall in all); then the capsule row
# 12 under the last, 64 tall: Bluetooth left, Do Not Disturb right (the
# gate has an adapter), 172 px each, 12 apart.
# A section's title row is centred 10 px down, its slider row 34: the low
# end's glyph 24 px in (the keyboard's and the speaker's are buttons), the
# track from 42 px in to 42 px off the right edge (272 px), or to 76 off it
# (238 px) where a module's own 24 px disc ends the row, flush right: the
# keyboard's room (the gate has a sensor) and the volume's output (the gate
# has two). A capsule's circle is 32 px into it, its words past 64. Its
# menus hang 6 px under the row.
sec_y() { echo $((N_TOP + 10 + $1 * 76)); }
level_cy() { echo $(($(sec_y "$1") + 34)); }
head_cy() { echo $(($(sec_y "$1") + 10)); }
GLYPH_X=$((PANEL_X + 24))
TRACK_X0=$((PANEL_X + 42))
track_x() { echo $((TRACK_X0 + ($1 == 0 ? 272 : 238) * $2 / 100)); } # track_x <level> <percent>
DISC_CX=$((PANEL_X + N_W - 16 - 12))
CAPS_Y=$((N_TOP + 216 + 12))
CAPS_CY=$((CAPS_Y + 32))
BT_DOT_X=$((PANEL_X + 32))
BT_WORDS_X=$((PANEL_X + 110))
DND_DOT_X=$((PANEL_X + 184 + 32))
DND_WORDS_X=$((PANEL_X + 184 + 110))
MENU_Y=$((CAPS_Y + 64 + 6))
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
	settle
}
tap() { # vkbd round-trips before it exits: the key has landed
	printf 'tap %s\n' "$1" | vk
	settle
}
wheel() {
	printf 'move %s %s\nsleep 60\nscroll 0 %s\nsleep 200\n' "$ROWX" "$ROWY" "$1" |
		vp
	settle
}
outside_click() { click "$((MON_W / 2))" "$((MON_H / 2))" 272; }

# the strip's left half (the tags and the tasks: no clock ticking) across a
# run of frames: "<distinct strips> <distinct frames> <returns>". A return:
# a frame shows a strip that an earlier frame showed and a later one left
# (a shimmer); content that only moves on never returns.
strip_states() { # strip_states <frame>...
	python3 - "$BAR_H" "$@" <<'PY'
import hashlib, sys
from PIL import Image
h = int(sys.argv[1]); frames, seen, last, returns = set(), set(), None, 0
for f in sys.argv[2:]:
    im = Image.open(f).convert('RGB')
    frames.add(hashlib.md5(im.tobytes()).digest())
    k = hashlib.md5(im.crop((0, 0, im.width // 2, h)).tobytes()).digest()
    if k != last and k in seen:
        returns += 1
    seen.add(k); last = k
print(len(seen), len(frames), returns)
PY
}
# mean abs difference of two frames' strips (left half), in 8-bit levels
# (a nested capture is flattened over black, as the host presents it)
strip_diff() { # strip_diff <a> <b>
	python3 - "$BAR_H" "$1" "$2" <<'PY'
import sys
from PIL import Image, ImageChops, ImageStat
h = int(sys.argv[1])
a, b = (Image.open(f).convert('L') for f in sys.argv[2:4])
box = (0, 0, min(a.width, b.width) // 2, h)
print(int(ImageStat.Stat(ImageChops.difference(a.crop(box), b.crop(box))).mean[0]))
PY
}

# ---- the bell, pointer-driven ------------------------------------------------
# The monolith's bell is a no-op on hover and toggles the shade on click;
# both are REAL pointer events over the widget. The right slot is [tray]
# [bell][control center][battery][clock], 20 px apart (shell/strip.cpp
# RIGHT_GAP), its cells meeting halfway across the gaps: a widget's glyph is
# the white ink inside its cell (`hyprctl awesome cells`, the gate's build)
# — found by pixel, so it proves the glyph draws where a press lands. White
# ink is every channel past 150, so neither the blue dot nor the accent
# counts — and so with the panels CLOSED only: an open panel lights its door
# in the accent.
ink_x() { # ink_x <frame> <widget> — the widget's ink center x (0 when none)
	python3 - "$1" "$(hq awesome cells | awk -v w="$2" '$1 == w { print $2 ":" $3; exit }')" <<'PY'
import sys
from PIL import Image
if ":" not in sys.argv[2]:
    print(0); raise SystemExit
x0, x1 = map(int, sys.argv[2].split(":"))
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
xs = [x for x in range(max(0, x0), min(im.width, x1)) if any(min(px[x, y]) > 150 for y in range(6, 23))]
print((xs[0] + xs[-1]) // 2 if xs else 0)
PY
}
bell_x() { ink_x "$1" bell; } # bell_x <frame> — the bell glyph's center x
# the bell's dot: col_dot, the accent (0xff00ccff), in the bell's cell —
# with the center CLOSED, when the bell itself is white
dot_px() { # dot_px <frame> — accent pixels on the bell (0 when no dot)
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
        n += b > 200 and g > 150 and r < 80
print(n)
PY
}
# strip_drawn <frame> — the strip finished its warm: the clock and the bell
# draw in their cells and the viewed tag draws in the accent at the left.
# Structure, not a brightness count: a count moves with the theme, the face
# and the hardware, and a mid-warm frame lacks whole glyphs.
strip_drawn() { [[ "$(ink_x "$1" clock)" -gt 0 && "$(bell_x "$1")" -gt 0 && "$(accent_px "$1" 8 34)" -gt 6 ]]; } # 一 is one stroke: ~10 px
bell_frame() { capture_nested "$STATE/bell-probe.png"; }

# ---- bounded waits ----------------------------------------------------------------
# shot_until <seconds> <png> <cmd...>: capture into png until cmd (which
# reads it) succeeds — the frame a change draws, however many frames that
# takes (the warm/draw gate, the raster worker, a D-Bus round trip). False
# at the bound, the last capture left for the checks. Poll for every check
# made on the frame, so a check of what must NOT draw reads the new frame.
shot_until() {
	local png=$2 now end
	now=${EPOCHREALTIME//[.,]/}
	end=$((now / 1000 + $1 * 1000))
	shift 2
	while :; do
		capture_nested "$png" && "$@" 2>/dev/null && return 0
		now=${EPOCHREALTIME//[.,]/}
		((now / 1000 < end)) || return 1
		sleep 0.05
	done
}
# bus_watch <log> <match>: a signal monitor on the nested's bus, returning
# once it is subscribed (busctl reports it after BecomeMonitor answered)
bus_watch() {
	( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 busctl --user monitor --match "$2" >"$1" 2>&1 & )
	until_ok 3 grep -q '^Monitoring bus message stream' "$1"
}
# boxes <class>: its windows' "x y w h", one a line, sorted
boxes() { clients | python3 -c "
import json,sys
print('\n'.join(sorted(' '.join(map(str, c['at'] + c['size'])) for c in json.load(sys.stdin) if c['class'] == sys.argv[1])))" "$1"; }
# settled <class> [count]: that many of its windows are mapped and their
# boxes held still across 150 ms — the map, the placement and the client's
# configure answer have landed
settled() {
	local want=${2:-1} a b n=50
	while ((n-- > 0)); do
		a="$(boxes "$1")"
		if [[ -n "$a" ]] && (($(grep -c . <<<"$a") == want)); then
			sleep 0.15
			b="$(boxes "$1")"
			[[ "$a" == "$b" ]] && return 0
		else
			sleep 0.1
		fi
	done
	return 1
}
# gone <address>: that window is closed; close_wait closes one and waits
gone() { ! clients | grep -qF "\"address\": \"$1\","; }
close_wait() { [[ -n "$1" ]] || return 0; dsp "hl.dsp.window.close({window=\"address:$1\"})"; until_ok 5 gone "$1"; }
# wait_class <class>: a window of that class (its app id) is mapped
has_class() { clients | grep -qF "\"class\": \"$1\","; }
# tile_props <address>: "<rounding> <no_shadow 0|1>" as the window draws
# them (windows/tile.hpp: a snapped or maximized window is square, no drop)
tile_props() {
	local r s
	r="$(hq getprop "address:$1" rounding)" s="$(hq getprop "address:$1" no_shadow)"
	case $s in true) s=1 ;; false) s=0 ;; esac
	echo "$r $s"
}
# class_addr <class>: the first such window's address (by app id: the shell
# in a foot rewrites its title at the first prompt)
class_addr() {
	clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class'] == sys.argv[1]), ''))" "$1"
}
wait_class() { until_ok 10 has_class "$1"; }
# until_is <seconds> <want> <cmd...>: poll until the command prints `want` —
# an asynchronous effect landing (an arrival, a sweep) — false on timeout.
# For final states only: a "nothing happened" check keeps a fixed settle.
until_is() {
	local n=$(($1 * 20)) want=$2
	shift 2
	while ((n-- > 0)); do
		[[ "$("$@" 2>/dev/null)" == "$want" ]] && return 0
		sleep 0.05
	done
	return 1
}
# until_ok <seconds> <cmd...>: poll until the command succeeds (a log line
# landing, a file filling) — the same contract as until_is
until_ok() {
	local n=$(($1 * 20))
	shift
	while ((n-- > 0)); do
		"$@" 2>/dev/null && return 0
		sleep 0.05
	done
	return 1
}
# sweep the model and wait until it is empty (the shade may stay open)
nclear() {
	hq awesome clear >/dev/null 2>&1
	local n=30
	while ((n-- > 0)) && [[ "$(st)" != *" live:0 "* ]]; do sleep 0.1; done
}

# ---- model strings -------------------------------------------------------------
st() { hq awesome state; }
bd() { hq awesome badge; }
# the open center's status line and rows (notify/center.cpp's describe):
# "<status> [undo] | <rows>"; rows_status the status alone
rows() { hq awesome center-rows; }
rows_status() { local r; r="$(rows)"; echo "${r%% |*}"; }
# the remembered DND's state.tsv value (empty: no row, DND off)
dndrow() { grep -a $'\tdnd$' "$AW_STATE" | cut -f2; }

# ---- the shell's menus ---------------------------------------------------------
# panel_extent <frame>: "<width> <x0> <x1> <top> <bottom>" of the open menu
# panels below the strip (zeros when none)
panel_extent() {
	python3 - "$1" <<'PY'
import sys
import numpy as np
from PIL import Image
# a column belongs to a panel when it holds more than 20 lit pixels below
# the strip, spanning more than 40 rows: its side rims (the glass has no
# tint, its inside dark over the nested's black)
lit = np.asarray(Image.open(sys.argv[1]).convert('RGB'))[29:].min(axis=2) > 5
n = lit.sum(axis=0)
top = lit.argmax(axis=0)
bot = lit.shape[0] - 1 - lit[::-1].argmax(axis=0)
xs = np.flatnonzero((n > 20) & (bot - top > 40))
if not xs.size:
    print("0 0 0 0 0"); sys.exit()
x0, x1 = int(xs[0]), int(xs[-1])
print(x1 - x0 + 1, x0, x1, int(top[xs].min()) + 29, int(bot[xs].max()) + 29)
PY
}
# srow <row> <separators above>: a shell menu row's center y (shell.hpp:
# rows 28 from y 37, separators 9)
srow() { echo $((37 + 14 + 28 * $1 + 9 * $2)); }
# bell_cx: the bell's cell center, from the gate's cell map (where a press
# lands; bell_x checks where the glyph draws)
bell_cx() { hq awesome cells | awk '$1 == "bell" { print int(($2 + $3) / 2); exit }'; }
# bell_open: right-click the bell; BRX0/BRX1 span its menu, BRY0/BRY1 its
# rims
bell_open() {
	click "$(bell_cx)" 13 273
	capture_nested "$STATE/bell-root.png"
	read -r _ BRX0 BRX1 BRY0 BRY1 <<<"$(panel_extent "$STATE/bell-root.png")"
}
# bell_pick <row>: click a row of the bell's flat options (shell/bell.cpp):
# its header (22), the two quiet switches (rows 0, 1), a separator, the
# second header, then the silenced rows (2 on)
bell_pick() {
	local y
	if (($1 < 2)); then y=$((37 + 22 + 14 + 28 * $1)); else y=$((37 + 22 + 56 + 9 + 22 + 14 + 28 * ($1 - 2))); fi
	click $(((BRX0 + BRX1) / 2)) "$y" 272
}
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
# nsend <notify-send args>: a notification on the nested's bus, back once
# the daemon answered — and it answers after the card arrived (notify/bus.cpp:
# Model::arrive, then the reply), so the next check needs no settle
nsend() { DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 10 notify-send "$@"; }

# ---- sends -----------------------------------------------------------------------
# A card with actions from a sender that STAYS on the bus, as an app does: a
# departed sender's card loses its actions (busctl's one-shot call would
# leave before the click). Hints are name/type/value triples (s b i u y).
# The sender lives until nfyact_done or its timeout.
NFYACT_SENDERS=()
# The senders that stay on the bus run in the background: each touches
# $SENT once the daemon answered its Notify (the card has arrived), and
# sent_wait takes that instead of a fixed settle.
SENT="$STATE/sent.flag"
sent_wait() {
	local n=100
	while ((n-- > 0)); do
		[[ -e "$SENT" ]] && { rm -f -- "$SENT"; return 0; }
		sleep 0.02
	done
	return 1
}
nfyact() {
	rm -f -- "$SENT"
	SENT="$SENT" DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 python3 - "$@" <<'PY' &
import os, sys
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
open(os.environ["SENT"], "w").close()
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
# A chat as Telegram Desktop's Linux manager posts one: a Notify per
# message, the chat's name the summary, "<b>Sender</b>\nmessage" in a group,
# the chat's picture as image-data, x-canonical-append, Open / Mark as read
# / Reply. Each message is "chat|sender|text" (no sender: a one-to-one
# chat's); a face file (64 px RGBA, raw) may follow a message as "@file".
# The ids the daemon answered go to the ids file, one a line. The sender
# stays on the bus (its cards keep their actions) until nfyact_done.
tgchat() { # tgchat <ids-file> <chat|sender|text>...
	DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 90 python3 - "$@" <<'PY' &
import html, sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def face(raw): # the pixels as one GBytes: an "ay" from bytes is built one element at a time
    ay = GLib.Variant.new_from_bytes(GLib.VariantType.new("ay"), GLib.Bytes.new(raw), True)
    return GLib.Variant.new_tuple(*(GLib.Variant("i", v) for v in (64, 64, 256)), GLib.Variant("b", True), GLib.Variant("i", 8), GLib.Variant("i", 4), ay)
plain = face(bytes((200, 120, 60, 255)) * 4096)
args, out = sys.argv[2:], open(sys.argv[1], "w")
for i, m in enumerate(args):
    if m.startswith("@"):
        continue
    chat, sender, text = m.split("|", 2)
    pic = face(open(args[i + 1][1:], "rb").read()) if i + 1 < len(args) and args[i + 1].startswith("@") else plain
    body = html.escape(text, quote=False)
    if sender:
        body = "<b>%s</b>\n%s" % (html.escape(sender, quote=False), body)
    hints = {"category": GLib.Variant("s", "im.received"), "desktop-entry": GLib.Variant("s", "org.telegram.desktop"),
             "x-canonical-append": GLib.Variant("s", "true"), "suppress-sound": GLib.Variant("b", True), "image-data": pic}
    r = bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
                      GLib.Variant("(susssasa{sv}i)", ("Telegram Desktop", 0, "", chat, body,
                                                       ["default", "Open", "mail-mark-read", "Mark as read", "inline-reply", "Reply"], hints, 30000)),
                      None, Gio.DBusCallFlags.NONE, -1, None)
    print(r.unpack()[0], file=out, flush=True)
GLib.MainLoop().run()
PY
	NFYACT_SENDERS+=($!)
}
# the fd.o signals a `busctl monitor` log saw, "<member> <id> <arg>" a line
sigs() { # sigs <monitor-log>
	python3 - "$1" <<'PY'
import re, sys
member, vals = None, []
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"Member=(\w+)", line)
    if m:
        if member and vals:
            print(member, *vals)
        member, vals = m.group(1), []
        continue
    v = re.match(r'\s*(?:UINT32|STRING) "?(.*?)"?;$', line)
    if v and member:
        vals.append(v.group(1))
if member and vals:
    print(member, *vals)
PY
}
# The same card from a sender that stays on the bus, as a chat app does: a
# card whose sender left (busctl's one-shot call above) is ownerless and
# opens no reply field. Sets REPLY_SENDER; the caller kills it.
conv_reply_sender() {
	rm -f -- "$SENT"
	SENT="$SENT" DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 python3 - "$1" "$2" "$3" <<'PY' &
import os, sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
              GLib.Variant("(susssasa{sv}i)", (sys.argv[1], 0, "", sys.argv[2], sys.argv[3], ["inline-reply", "Reply"],
                                               {"category": GLib.Variant("s", "im.received")}, 60000)),
              None, Gio.DBusCallFlags.NONE, -1, None)
open(os.environ["SENT"], "w").close()
GLib.MainLoop().run()
PY
	REPLY_SENDER=$!
}

# ---- pixels ------------------------------------------------------------------------
panel_bottom() { # panel_bottom <frame> — the center column's last lit row (0 when empty)
	# from inside the first card down: the heading row above it is left
	# out, and the cards stand 8 px apart, 16 where a group of the ranking
	# ends (a gap past 20 rows ends it)
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
    elif all(not filled(yy) for yy in range(y, min(y + 20, H))):
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
lifted_px() { # lifted_px <frame> <x0> <x1> [y0 y1] — the accent lifted toward white (hover) in a box
	python3 - "$1" "$2" "$3" "${4:-0}" "${5:-$BAR_H}" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
n = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        r, g, b = px[x, y]
        if b > 200 and g > 190 and 80 <= r <= 150:
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
# a ground under a mark on the bar, flattened over the nested's black: the
# pill (white 16 %) settles near 41, the bar's cell (8 %) near 20 — grey,
# every channel within [lo, hi]
ground_px() { # ground_px <frame> <x0> <x1> <y0> <y1> <lo> <hi> — grey ground pixels in a box
	python3 - "$@" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
x0, x1, y0, y1, lo, hi = (int(v) for v in sys.argv[2:8])
print(sum(1 for y in range(y0, y1) for x in range(x0, x1) if lo <= min(px[x, y]) and max(px[x, y]) <= hi and max(px[x, y]) - min(px[x, y]) <= 10))
PY
}
# a pill lifted over a menu's glass: pixels in a box whose every channel
# sits 20 over the same row's glass just outside it (x out). Relative: the
# glass blurs in what lies under it (over a terminal it reads teal, near
# 31; the pill over it near 70)
pill_px() { # pill_px <frame> <x out> <x0> <x1> <y0> <y1> — lifted pixels in a box
	python3 - "$@" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
xo, x0, x1, y0, y1 = (int(v) for v in sys.argv[2:7])
print(sum(1 for y in range(y0, y1) for x in range(x0, x1) if all(px[x, y][c] >= px[xo, y][c] + 20 for c in range(3))))
PY
}
