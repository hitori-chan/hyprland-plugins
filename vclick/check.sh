#!/usr/bin/env bash
# vclick's nested check: a click lands where a window capture shows it,
# under a covering window, in a popup, a scroll steps exactly, a glide
# moves at its pace and ends exactly on target, the mouse mode keeps the
# hover and its cursor at rest (under any window above, and taken by the
# user's pointer coming into the app), the tap mode leaves and fades, the
# lockscreen lets it reach its window while the user's input stays the
# lockscreen's, and the user's pointer never notices; a key reaches its
# window alone, and the user's keyboard never notices. Runs a nested
# Hyprland in the gate's private host
# (awesome/gate/launch.sh) with only vclick loaded; the live session is
# never touched.
#
# usage: make -C vclick && bash vclick/check.sh
# Two GTK3 clients (devtools/gtkgrid.py, each with a protocol trace):
# A is the target, B covers A's lower right and is the bystander. A
# virtual pointer (devtools/vptr) is "the user's" mouse, a virtual
# keyboard (devtools/vkbd) their keyboard.
set -u

source "$(dirname "${BASH_SOURCE[0]}")/../awesome/gate/memcap.sh" # the run's memory cap

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
GATE="$REPO/awesome/gate"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"

# one nested at a time, shared with the gate (launch.sh ends any prior nested)
mkdir -p "$HARNESS"
exec 9>"$HARNESS/.lock"
flock -n 9 || { echo "check: another gate/probe/dev run holds $HARNESS/.lock" >&2; exit 2; }
export HYPR_HARNESS_LOCKED=1

T=/tmp/hypr-gate/vclick
rm -rf "$T" && mkdir -p "$T"
[[ -f "$HERE/vclick.so" ]] || { echo "check: build first: make -C vclick" >&2; exit 2; }
# the nested maps a copy: a rebuild never overwrites a mapped .so
SO="$T/vclick-$$.so"
cp "$HERE/vclick.so" "$SO"

# the cursor theme vclick draws named shapes from: two of its own, never
# the session's: default (azure, hotspot 2,2) and text (chartreuse, 10,10)
xcursor() { # xcursor <file> <side> <hot x> <hot y> <r,g,b>: one solid image at nominal size 24
	python3 - "$@" <<'PY'
import struct, sys
path, side, hx, hy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
r, g, b = map(int, sys.argv[5].split(","))
px = struct.pack("<I", 0xff000000 | r << 16 | g << 8 | b) * (side * side)
chunk = struct.pack("<9I", 36, 0xfffd0002, 24, 1, side, side, hx, hy, 0) + px
open(path, "wb").write(struct.pack("<4I", 0x72756358, 16, 0x10000, 1) + struct.pack("<3I", 0xfffd0002, 24, 28) + chunk)
PY
}
mkdir -p "$T/icons/vtheme/cursors"
xcursor "$T/icons/vtheme/cursors/default" 20 2 2 0,128,255
xcursor "$T/icons/vtheme/cursors/text" 20 10 10 128,255,0

MON_W=1280 MON_H=800
cat >"$T/nested.lua" <<EOF
hl.env("XCURSOR_PATH", "$T/icons")
hl.env("XCURSOR_THEME", "vtheme")
hl.env("XCURSOR_SIZE", "24")
hl.monitor({ output = "", mode = "${MON_W}x${MON_H}@60", position = "0x0", scale = 1 })
hl.config({
	general = { gaps_in = 0, gaps_out = 0, border_size = 1 },
	animations = { enabled = false },
	decoration = { shadow = { enabled = false }, blur = { enabled = false } },
	misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
})
hl.plugin.load("$SO")
EOF

VP=""
cleanup() {
	exec 7>&- 6>&- 5>&- 2>/dev/null
	[[ -n "$VP" ]] && kill "$VP" 2>/dev/null
	bash "$GATE/stop.sh" >/dev/null 2>&1
}
trap cleanup EXIT

HYPR_CFG="$T/nested.lua" bash "$GATE/launch.sh" >"$T/launch.log" 2>&1 || { cat "$T/launch.log" >&2; exit 1; }
SIG="$(cat "$HARNESS/nested.sig" 2>/dev/null)" WL="$(cat "$HARNESS/nested.wl" 2>/dev/null)"
# never the live session: a nested signature owned by a marked nested, and its own display
[[ "$SIG" =~ ^[[:alnum:]_.-]+$ && "$SIG" != "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] || { echo "check: no nested signature" >&2; exit 1; }
NPID="$(head -1 "${XDG_RUNTIME_DIR:?}/hypr/$SIG/hyprland.lock" 2>/dev/null)"
grep -Fzxq -- "AW_GATE_NESTED=1" "/proc/$NPID/environ" 2>/dev/null || { echo "check: $SIG is not a harness nested" >&2; exit 1; }
[[ "$WL" =~ ^wayland-[0-9]+$ && "$WL" != "${WAYLAND_DISPLAY:-}" ]] || { echo "check: no nested display" >&2; exit 1; }

hq() { hyprctl -i "$SIG" "$@"; }
dsp() { hq dispatch "$1" >/dev/null 2>&1; }
vptr() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1; }
until_ok() { # until_ok <seconds> <cmd...>
	local n=$(($1 * 20))
	shift
	while ((n-- > 0)); do
		"$@" 2>/dev/null && return 0
		sleep 0.05
	done
	return 1
}

PASS=0 FAIL=0
ok() { PASS=$((PASS + 1)) && echo "  ok    $*"; }
bad() { FAIL=$((FAIL + 1)) && echo "  FAIL  $*"; }
expect() { # expect <description> <got> <want>
	if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1: got '$2', want '$3'"; fi
}

# the user's mouse: one device for the whole run
mkfifo "$T/vp.fifo"
vptr <"$T/vp.fifo" &
VP=$!
exec 7>"$T/vp.fifo"
ptr() { printf '%s\n' "$@" >&7; }
cursor_at() { [[ "$(hq cursorpos)" == "$1, $2" ]]; }
park() { # park <x> <y>: the user's pointer to a global spot, settled
	ptr "move $1 $2"
	until_ok 3 cursor_at "$1" "$2"
}
# the user's keyboard: up before the clients, so they bind a wl_keyboard
kbds() { hq devices -j | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["keyboards"]))'; }
kbd_up() { (($(kbds) > 0)); }
exec 6> >(WAYLAND_DISPLAY="$WL" "$REPO/devtools/vkbd" >/dev/null 2>&1)
kb() { printf '%s\n' "$@" >&6; }
until_ok 3 kbd_up || { echo "check: the virtual keyboard never came up" >&2; exit 1; }

# --- the clients ----------------------------------------------------------
grids() { # address stableId at.x at.y size.x size.y floating, per gtkgrid
	hq clients -j | python3 -c '
import json, sys
for c in json.load(sys.stdin):
    if c["title"] == "gtkgrid":
        print(c["address"], c["stableId"], *c["at"], *c["size"], int(c["floating"]))'
}
open_grid() { # open_grid <name> [cursor]: spawn, wait for READY and the window; prints its address
	local before
	before="$(grids | cut -d' ' -f1)"
	dsp "hl.dsp.exec_cmd('python3 $REPO/devtools/gtkgrid.py $T/$1.log wayland $T/$1.wl ${2:-}')"
	until_ok 15 grep -q READY "$T/$1.log" || return 1
	for _ in $(seq 1 200); do
		local new
		new="$(comm -13 <(sort <<<"$before") <(grids | cut -d' ' -f1 | sort) | head -1)"
		[[ -n "$new" ]] && { printf '%s\n' "$new"; return 0; }
		sleep 0.05
	done
	return 1
}
field() { grids | awk -v a="$1" -v f="$2" '$1 == a { print $f }'; } # field <address> <column>

A="$(open_grid a cursor)" || { echo "check: client A never mapped" >&2; exit 1; } # A: a custom cursor image, as a web page's
B="$(open_grid b shapes)" || { echo "check: client B never mapped" >&2; exit 1; } # B: text over yellow, no cursor over red
[[ "$(field "$A" 7)" == 1 ]] || dsp "hl.dsp.window.float({ action = 'enable', window = 'address:$A' })"
[[ "$(field "$B" 7)" == 1 ]] || dsp "hl.dsp.window.float({ action = 'enable', window = 'address:$B' })"
dsp "hl.dsp.window.move({ x = 60, y = 60, window = 'address:$A' })"
dsp "hl.dsp.window.move({ x = 260, y = 200, window = 'address:$B' })"
placed() { [[ "$(field "$A" 3),$(field "$A" 4) $(field "$B" 3),$(field "$B" 4)" == "60,60 260,200" ]]; }
until_ok 3 placed
expect "the clients float where placed (A at 60,60, B at 260,200)" "$(field "$A" 3),$(field "$A" 4) $(field "$B" 3),$(field "$B" 4)" "60,60 260,200"

# --- where the blocks are DRAWN: A's window capture, as the user takes it --
# The nested renders on damage: a second device jitters the pointer (out
# and back) until grim has a frame. Captures happen before the user's
# pointer is parked for a check.
wincap() { # wincap <stableId> <png>
	local gpid jpid
	for _ in 1 2 3; do
		timeout 8 env WAYLAND_DISPLAY="$WL" grim -l 1 -T "$1" "$2" >/dev/null 2>&1 &
		gpid=$!
		(while kill -0 "$gpid" 2>/dev/null; do
			printf 'rel 2 2\nsleep 25\nrel -2 -2\nsleep 25\n'
			sleep 0.05
		done | vptr) &
		jpid=$!
		wait "$gpid" && { wait "$jpid" 2>/dev/null; return 0; }
		wait "$jpid" 2>/dev/null
		sleep 0.2
	done
	return 1
}
COLORS="red=255,0,0 green=0,255,0 blue=0,0,255 yellow=255,255,0 magenta=255,0,255 cyan=0,255,255 button=128,64,0"
wincap "$(field "$A" 2)" "$T/a.png" || { echo "check: no capture of A" >&2; exit 1; }
python3 "$REPO/devtools/findcolors.py" "$T/a.png" $COLORS >"$T/a.blocks"
at() { awk -v n="$1" '$1 == n { print $2, $3 }' "$T/a.blocks"; } # at <color>: x y in A's capture
for c in red green blue yellow magenta cyan button; do
	[[ "$(at "$c")" != "-1 -1" ]] || { echo "check: $c not drawn in A's capture" >&2; cat "$T/a.blocks" >&2; exit 1; }
done
echo "  (A's capture $(python3 -c 'import sys; from PIL import Image; print("%dx%d" % Image.open(sys.argv[1]).size)' "$T/a.png"); red at $(at red), cyan at $(at cyan))"

# --- protocol traces --------------------------------------------------------
hits() { grep -c "^$2 $3\$" "$T/$1.log"; } # hits <client> HIT|MENU <color>
more() { (($(hits "$1" "$2" "$3") > $4)); } # more <client> <tag> <color> <n>: past n lines
atleast() { (($(hits "$1" "$2" "$3") >= $4)); }
lines() { wc -l <"$1"; }
quiet() { # quiet <file>: wait until the file stops growing (100 ms)
	local a b
	for _ in $(seq 1 60); do
		a="$(lines "$1")" && sleep 0.1 && b="$(lines "$1")"
		[[ "$a" == "$b" ]] && return 0
	done
	return 1
}
events() { # events <client> <from-line>: wl_pointer events the client received since
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.(enter|leave|motion|button|axis)\(' |
		sed 's/.*\.//; s/($//' | tr '\n' ' ' | sed 's/ $//'
}
detents() { # detents <client> <from-line>: "axis:value120" per wheel step since (axis_discrete d as d*120)
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.axis_(value120|discrete)\([0-9]+, -?[0-9]+' |
		awk -F'[(, ]+' '{ v = $3; if ($1 ~ /discrete$/) v *= 120; printf "%s%s:%s", (n++ ? " " : ""), $2, v }'
}
axes() { # axes <client> <from-line>: "axis:value" per wl_pointer.axis since
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.axis\([0-9]+, [0-9]+, -?[0-9.]+' |
		awk -F'[(, ]+' '{ printf "%s%s:%g", (n++ ? " " : ""), $3, $4 }'
}
gaps() { # gaps <client> <from-line>: ms between consecutive wl_pointer.axis events since (their time argument)
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.axis\([0-9]+,' | grep -oE '[0-9]+,$' |
		awk -F, 'NR > 1 { printf "%s%d", (n++ ? " " : ""), $1 - p } { p = $1 }'
}
within() { # within <lo> <hi> <gaps...>: every gap in [lo, hi], and at least one
	local lo=$1 hi=$2 g
	shift 2
	(($# > 0)) || return 1
	for g in "$@"; do ((g >= lo && g <= hi)) || return 1; done
}
idle() { [[ "$(vclick status)" == idle* ]]; }
scrolls() { grep -c "^SCROLL $2 $3\$" "$T/$1.log"; } # scrolls <client> <color> <direction>
scrolled() { (($(scrolls "$1" "$2" "$3") >= $4)); }
new_scrolls() { # new_scrolls <client> <from-log-line> <color>: GTK's directions since
	tail -n +"$(($2 + 1))" "$T/$1.log" | awk -v c="$3" '$1 == "SCROLL" && $2 == c { printf "%s%s", (n++ ? " " : ""), $3 }'
}
has_new_scroll() { [[ -n "$(new_scrolls "$@")" ]]; }
scroll_ok() { # scroll_ok <client> <color> <direction> <before> <steps>: exactly that many steps, after the client is quiet
	until_ok 3 scrolled "$1" "$2" "$3" $(($4 + $5)) || true
	quiet "$T/$1.wl"
	expect "  $1 scrolled $2 $3 $5 step(s)" "$(($(scrolls "$1" "$2" "$3") - $4))" "$5"
}
last_xy() { # last_xy <client> enter|motion <from-line>: integer x y of its last such wl_pointer event
	tail -n +"$(($3 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -E "wl_pointer[#@][0-9]+\\.$2\\(" | tail -1 |
		sed -E 's/.*, (-?[0-9]+)\.[0-9]+, (-?[0-9]+)\.[0-9]+\)$/\1 \2/'
}
fmt_xy() { printf '%s %s' "$1" "$2"; }
vclick() { hq vclick "$@" 2>&1; }

# --- 1. the command --------------------------------------------------------
echo "== vclick"
expect "loaded" "$(hq plugin list | grep -c 'Plugin vclick')" 1
expect "no arguments: usage" "$(vclick | head -1 | cut -c1-13)" "usage: vclick"
expect "no verb: usage" "$(vclick "address:$A" 10 10 | head -1 | cut -c1-13)" "usage: vclick"
expect "unknown window" "$(vclick click address:0x1 10 10)" "error: window not found"
expect "outside the window" "$(vclick click "address:$A" 5000 10 | sed 's/ (.*//')" "error: 5000,10 is outside the window"
expect "unknown option" "$(vclick click "address:$A" 10 10 thumb | head -1)" "error: 'thumb' is not an option of click"
expect "an option twice" "$(vclick click "address:$A" 10 10 right left | head -1)" "error: 'left': that option is given twice"
expect "a scroll needs a direction" "$(vclick scroll "address:$A" 10 10 3 | head -1)" "error: a scroll needs its direction: up, down, left or right"
expect "steps bound" "$(vclick scroll "address:$A" 10 10 down 21 | head -1)" "error: '21': steps must be 1-20"
expect "pace bound" "$(vclick scroll "address:$A" 10 10 down 2 1001ms | head -1)" "error: '1001ms': the pace must be 0ms-1000ms"
expect "a pace is for a scroll only" "$(vclick click "address:$A" 10 10 50ms | head -1)" "error: '50ms' is not an option of click"
expect "a move takes no count" "$(vclick move "address:$A" 10 10 2 | head -1)" "error: '2' is not an option of move"
expect "a target's size bound" "$(vclick click "address:$A" 10 10 size:2 | head -1)" "error: 'size:2': the target's size must be 4-1000 px"
expect "status: idle" "$(vclick status)" idle
expect "where: nowhere yet" "$(vclick where)" nowhere
expect "the mouse is the default mode" "$(vclick mode)" mouse
expect "a bad mode" "$(vclick mode fly | head -1 | cut -c1-13)" "usage: vclick"
expect "a bare 0x address is an address" "$(vclick image "$A")" "$(vclick image "address:$A")"
lua() { hq repl "return $1" 2>&1; }
expect "lua: a query is its answer" "$(lua 'hl.plugin.vclick.status()')" idle
expect "lua: an error is nil and its line" "$(lua "hl.plugin.vclick.click('address:0x1', 10, 10)")" $'nil\terror: window not found'
expect "lua: a malformed call too, without the usage" "$(lua "hl.plugin.vclick.scroll('address:$A', 10, 10, 'down', 21)")" $'nil\terror: \'21\': steps must be 1-20'
expect "lua: a number where hyprctl takes a word" "$(lua "hl.plugin.vclick.click('address:$A', 5000.5, 10)" | sed 's/ (.*//')" $'nil\terror: 5000.5,10 is outside the window'
expect "lua: an argument hyprctl could not take" "$(lua "hl.plugin.vclick.click({}, 10, 10)")" $'nil\terror: argument 1 is a table: a string or a number, as hyprctl takes it'
# the pace for the check: a profile file of its own, never the user's;
# no overshoot, so a glide's time is Fitts's law exactly
printf 'overshoot_p=0\n' >"$T/even.profile"
expect "a profile from a file" "$(vclick profile reload "$T/even.profile" | head -1)" "loaded $T/even.profile"
expect "  read back" "$(vclick profile | head -1)" "fitts a 50 ms, b 120 ms/bit (W 32 px); bow 0.06 +- 0.04; speed peak at 0.45; overshoot 0% of moves >= 300 px, by 3%; dwell 80 ms; 125 Hz"
# the exact event sequences below are the tap mode's; the mouse mode has its own section
expect "the tap mode" "$(vclick mode tap)" tap

# --- 2. another app under the user's pointer --------------------------------
echo "== the user's pointer is on B (another client); A is clicked"
BX=$(($(field "$B" 3) + $(field "$B" 5) / 2)) BY=$(($(field "$B" 4) + $(field "$B" 6) / 2))
park "$BX" "$BY"
quiet "$T/a.wl" && quiet "$T/b.wl"
POS="$(hq cursorpos)" ACTIVE="$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')"
AM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")"
for c in red cyan; do # opposite corners: an offset or a scale error misses one; cyan is under B
	n="$(hits a HIT "$c")"
	expect "click $c at $(at "$c")" "$(vclick click "address:$A" $(at "$c"))" ok
	until_ok 3 more a HIT "$c" "$n" && ok "A hit $c" || bad "A never hit $c ($(tail -3 "$T/a.log" | tr '\n' ' '))"
done
quiet "$T/a.wl" && quiet "$T/b.wl"
expect "A heard two whole clicks" "$(events a "$AM")" "enter motion button button leave enter motion button button leave"
expect "B heard nothing" "$(events b "$BM")" ""
expect "the cursor never moved" "$(hq cursorpos)" "$POS"
expect "the focus never moved" "$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')" "$ACTIVE"

echo "== scroll, the user's pointer still on B"
AM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")"
n="$(scrolls a magenta down)"
expect "scroll down 3 steps over magenta" "$(vclick scroll "address:$A" $(at magenta) down 3)" ok
scroll_ok a magenta down "$n" 3
n="$(scrolls a cyan up)"
expect "scroll up 2 steps over cyan (under B)" "$(vclick scroll "address:$A" $(at cyan) up 2)" ok
scroll_ok a cyan up "$n" 2
# GTK3 (3.24.52) names a +x wheel step "left", a real wheel's too: the
# horizontal step is held against the user's own wheel in the next section,
# not against GTK's name
GL="$(lines "$T/a.log")"
expect "scroll right over green" "$(vclick scroll "address:$A" $(at green) right)" ok
until_ok 3 has_new_scroll a "$GL" green || true
quiet "$T/a.wl"
VDIR="$(new_scrolls a "$GL" green)"
expect "  a scrolled green 1 step, horizontally" "$(sed 's/right/x/; s/left/x/' <<<"$VDIR")" x
quiet "$T/b.wl"
expect "A heard each step as its own enter, axis, leave (paced)" "$(events a "$AM")" "$(printf 'enter motion axis leave%.0s ' 1 2 3 4 5 6 | sed 's/ $//')"
expect "  each step one detent (axis:value120)" "$(detents a "$AM")" "0:120 0:120 0:120 0:-120 0:-120 1:120"
expect "  each step 15 units (axis:value)" "$(axes a "$AM")" "0:15 0:15 0:15 0:-15 0:-15 1:15"
expect "B heard nothing" "$(events b "$BM")" ""
expect "the cursor never moved" "$(hq cursorpos)" "$POS"
expect "the focus never moved" "$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')" "$ACTIVE"
G="$(gaps a "$AM")"
within 48 150 $(cut -d' ' -f1-2 <<<"$G") && ok "  the default pace: 50 ms a step ($G)" || bad "  the default pace: gaps $G, want 2 of 48-150 ms first"

echo "== the pace"
AM="$(lines "$T/a.wl")" n="$(scrolls a magenta down)"
expect "0ms: one burst" "$(vclick scroll "address:$A" $(at magenta) down 3 0ms)" ok
scroll_ok a magenta down "$n" 3
expect "  one enter, the steps, one leave" "$(events a "$AM")" "enter motion axis axis axis leave"
G="$(gaps a "$AM")"
within 0 2 $G && ok "  a millisecond apart ($G)" || bad "  gaps $G, want 0-2 ms"
AM="$(lines "$T/a.wl")" n="$(scrolls a magenta up)"
expect "200ms: a slow run" "$(vclick scroll "address:$A" $(at magenta) up 4 200ms)" ok
expect "  status while it runs" "$(vclick status)" "scrolling 1/4"
expect "  another scroll meanwhile is busy" "$(vclick scroll "address:$A" $(at magenta) down | cut -c1-28)" "error: busy: still scrolling"
expect "  so is a click elsewhere" "$(vclick click "address:$B" 10 10 | cut -c1-28)" "error: busy: still scrolling"
expect "  and so is hide" "$(vclick hide | cut -c1-28)" "error: busy: still scrolling"
expect "  and so is a key" "$(vclick key "address:$A" Next | cut -c1-28)" "error: busy: still scrolling"
until_ok 3 idle || true
expect "  status after it" "$(vclick status)" idle
scroll_ok a magenta up "$n" 4
G="$(gaps a "$AM")"
within 195 400 $G && ok "  200 ms apart ($G)" || bad "  gaps $G, want 3 of 195-400 ms"

echo "== a run ends when its window does"
C="$(open_grid c)" || bad "client C never mapped"
if [[ -n "$C" ]]; then
	expect "start a run into C" "$(vclick scroll "address:$C" 50 50 down 20 200ms)" ok
	sleep 0.5
	pkill -f "gtkgrid.py $T/c.log"
	until_ok 3 idle || true
	expect "  the run stopped, and says why" "$(vclick status | sed -E 's#after [0-9]+/20: error: .*#after N/20: error: …#')" "idle; the last scroll stopped after N/20: error: …"
	gone() { [[ -z "$(field "$C" 1)" ]]; }
	until_ok 3 gone || true
fi

AM="$(lines "$T/a.wl")" n="$(hits a HIT green)"
expect "a double click" "$(vclick click "address:$A" $(at green) double)" ok
until_ok 3 atleast a HIT green $((n + 2)) && ok "A saw both presses" || bad "A saw $(($(hits a HIT green) - n)) presses"
quiet "$T/a.wl"
expect "  as one enter, two clicks, one leave" "$(events a "$AM")" "enter motion button button button button leave"
AM="$(lines "$T/a.wl")"
expect "a right click" "$(vclick click "address:$A" $(at blue) right)" ok
quiet "$T/a.wl"
expect "  is button 273" "$(tail -n +"$((AM + 1))" "$T/a.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.button\([0-9]+, [0-9]+, [0-9]+' | awk -F', ' '{ print $3 }' | sort -u)" 273

# --- keys -----------------------------------------------------------------------
echo "== a key into A; the user's keyboard is in B (another client)"
kevents() { # kevents <client> <from-line>: wl_keyboard events the client received since
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_keyboard[#@][0-9]+\.(enter|leave|key|modifiers)\(' |
		sed 's/.*\.//; s/($//' | tr '\n' ' ' | sed 's/ $//'
}
keys() { grep -c "^KEY $2 $3\$" "$T/$1.log"; } # keys <client> <keyval> <mods|->: GTK's key presses
keyed() { (($(keys "$1" "$2" "$3") > $4)); }
active() { hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))'; }
active_is() { [[ "$(active)" == "$1" ]]; }
dsp "hl.dsp.focus({ window = 'address:$B' })"
until_ok 3 active_is "$B" || bad "B never took the keyboard"
park "$BX" "$BY" # a focus may warp the cursor
quiet "$T/a.wl" && quiet "$T/b.wl"
AM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")" n="$(keys a Page_Down -)"
expect "key Next (PageDown) into A" "$(vclick key "address:$A" Next)" ok
until_ok 3 keyed a Page_Down - "$n" && ok "  A read Page_Down, no modifier" || bad "  A never read Page_Down ($(tail -2 "$T/a.log" | tr '\n' ' '))"
quiet "$T/a.wl"
expect "  A heard an enter, the press and the release, and a leave" "$(kevents a "$AM")" "enter modifiers key key modifiers leave"
expect "  B, which has the user's keyboard, heard nothing" "$(kevents b "$BM")" ""
expect "  the keyboard focus never moved" "$(active)" "$B"
AM="$(lines "$T/a.wl")" n="$(keys a A ctrl+shift)"
expect "key a with ctrl+shift" "$(vclick key "$A" a ctrl+shift)" ok
until_ok 3 keyed a A ctrl+shift "$n" && ok "  A read ctrl+shift+A" || bad "  A never read ctrl+shift+A ($(tail -2 "$T/a.log" | tr '\n' ' '))"
quiet "$T/a.wl"
expect "  each press has its release: no key stays held" \
	"$(tail -n +"$((AM + 1))" "$T/a.wl" | grep -v ' -> ' | grep -oE 'wl_keyboard[#@][0-9]+\.key\([0-9]+, [0-9]+, [0-9]+, [0-9]+' | awk -F', ' '{ s[$4]++ } END { print s[1] + 0, s[0] + 0 }')" "1 1"
n="$(keys a Page_Up -)"
expect "lua: a key is true" "$(lua "hl.plugin.vclick.key('$A', 'Prior')")" true
until_ok 3 keyed a Page_Up - "$n" && ok "  A read Page_Up" || bad "  A never read Page_Up"
n="$(hits a HIT red)"
expect "lua: a click is true" "$(lua "hl.plugin.vclick.click('address:$A', $(at red | tr ' ' ','), 'jump')")" true
until_ok 3 more a HIT red "$n" && ok "  A hit red" || bad "  A never hit red"
expect "an unknown key" "$(vclick key "address:$A" NoSuchKey | head -1)" "error: 'NoSuchKey' is not a key"
expect "an unknown modifier" "$(vclick key "address:$A" a hyper | head -1)" "error: 'hyper' is not a modifier"
expect "a key needs its key" "$(vclick key "address:$A" | head -1 | cut -c1-13)" "usage: vclick"
expect "a key to no window" "$(vclick key address:0x1 Next)" "error: window not found"

echo "== a key into A; the user's keyboard is in A"
dsp "hl.dsp.focus({ window = 'address:$A' })"
until_ok 3 active_is "$A" || bad "A never took the keyboard"
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")" n="$(keys a Page_Up -)"
expect "key Prior (PageUp) into A" "$(vclick key "address:$A" Prior)" ok
until_ok 3 keyed a Page_Up - "$n" && ok "  A read Page_Up" || bad "  A never read Page_Up"
quiet "$T/a.wl"
expect "  no enter and no leave around it: the user's focus stays as it was" "$(kevents a "$AM")" "modifiers key key modifiers"
expect "  the keyboard focus never moved" "$(active)" "$A"
n="$(keys a a -)"
kb "press 30" # the user holds a key in A
until_ok 3 keyed a a - "$n" || bad "the user's key never reached A"
expect "the user holds a key in A: busy" "$(vclick key "address:$A" Next)" "error: busy: the user holds a key in this app; retry later"
kb "release 30"
dsp "hl.dsp.focus({ window = 'address:$B' })"
until_ok 3 active_is "$B" || bad "B never took the keyboard back"
park "$BX" "$BY"

# --- 3. the same app under the user's pointer -------------------------------
echo "== the user's pointer is on A itself; A is clicked elsewhere"
read -r RX RY <<<"$(at red)"
UX=$((60 + RX)) UY=$((60 + RY)) # A's red block, globally (A sits at 60,60)
park "$UX" "$UY"
quiet "$T/a.wl"
read -r LX LY <<<"$(last_xy a motion 0)" # where A heard the user's pointer
# A moves 10 px under the still cursor: no motion need follow, so the
# seat's last motion may be stale; the step back in must land where the
# cursor really is over A now
dsp "hl.dsp.window.move({ x = 70, y = 70, window = 'address:$A' })"
moved() { [[ "$(field "$A" 3),$(field "$A" 4)" == "70,70" ]]; }
until_ok 3 moved || bad "A never moved to 70,70"
quiet "$T/a.wl"
POS="$(hq cursorpos)" AM="$(lines "$T/a.wl")" n="$(hits a HIT yellow)"
expect "click yellow" "$(vclick click "address:$A" $(at yellow))" ok
until_ok 3 more a HIT yellow "$n" && ok "A hit yellow" || bad "A never hit yellow"
quiet "$T/a.wl"
expect "A's pointer stepped out and back" "$(events a "$AM" | sed 's/ motion$//')" "leave enter motion button button leave enter"
expect "  back in under the cursor (A moved 10 px under it)" "$(last_xy a enter "$AM")" "$(fmt_xy $((LX - 10)) $((LY - 10)))"
expect "the cursor never moved" "$(hq cursorpos)" "$POS"
n="$(hits a HIT red)"
ptr "press 272" "sleep 30" "release 272"
until_ok 3 more a HIT red "$n" && ok "the user's own click still lands on red" || bad "the user's own click missed red"
GL="$(lines "$T/a.log")"
ptr "scroll 1 15" # the user's own wheel through the seat: one +x (right) detent
until_ok 3 has_new_scroll a "$GL" red || true
expect "vclick's scroll-right reads in GTK as the user's own +x wheel step" "$VDIR" "$(new_scrolls a "$GL" red)"
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")" n="$(scrolls a yellow down)"
expect "scroll down over yellow" "$(vclick scroll "address:$A" $(at yellow) down)" ok
scroll_ok a yellow down "$n" 1
expect "  A's pointer stepped out and back" "$(events a "$AM" | sed 's/ motion$//')" "leave enter motion axis leave enter"
expect "  back in under the cursor" "$(last_xy a enter "$AM")" "$(fmt_xy $((LX - 10)) $((LY - 10)))"
expect "the cursor never moved" "$(hq cursorpos)" "$POS"

echo "== the user holds a button in A"
quiet "$T/a.wl"
n="$(hits a HIT red)"
ptr "press 272"
until_ok 3 more a HIT red "$n" || bad "the user's press never reached A"
expect "A is busy while the user's button is down" "$(vclick click "address:$A" $(at yellow) | cut -c1-11)" "error: busy"
n="$(hits b HIT magenta)"
read -r MX MY <<<"$(at magenta)" # B is the same app as A: its blocks sit alike
expect "B (another app) still clicks" "$(vclick click "address:$B" "$MX" "$MY")" ok
until_ok 3 more b HIT magenta "$n" && ok "B hit magenta" || bad "B never hit magenta"
ptr "release 272"
quiet "$T/a.wl"
n="$(scrolls a yellow down)" m="$(hits a HIT red)"
AM="$(lines "$T/a.wl")"
expect "a paced run into A (100 ms)" "$(vclick scroll "address:$A" $(at yellow) down 3 100ms)" ok
ptr "press 272" "sleep 300" "release 272"
until_ok 3 more a HIT red "$m" || bad "the user's press never reached A"
until_ok 4 idle || true
expect "  finishes" "$(vclick status)" idle
scroll_ok a yellow down "$n" 3
G="$(gaps a "$AM")"
(($(tr ' ' '\n' <<<"$G" | sort -n | tail -1) >= 200)) && ok "  a step waited out the user's button ($G)" || bad "  no step waited: gaps $G"

# --- 4. a popup -------------------------------------------------------------
echo "== a popup: A's menu"
n="$(hits a MENUBUTTON True)"
expect "click the menu button" "$(vclick click "address:$A" $(at button))" ok
until_ok 3 more a MENUBUTTON True "$n" && ok "the menu opened" || bad "the menu never opened"
fullcap() { # the whole nested output: the menu may hang below A's box
	local gpid jpid
	timeout 8 env WAYLAND_DISPLAY="$WL" grim -l 1 "$1" >/dev/null 2>&1 &
	gpid=$!
	(while kill -0 "$gpid" 2>/dev/null; do
		printf 'rel 2 2\nsleep 25\nrel -2 -2\nsleep 25\n'
		sleep 0.05
	done | vptr) &
	jpid=$!
	wait "$gpid"
	local r=$?
	wait "$jpid" 2>/dev/null
	return $r
}
ORANGE=""
for _ in 1 2 3 4 5; do
	fullcap "$T/menu.png" && ORANGE="$(python3 "$REPO/devtools/findcolors.py" "$T/menu.png" orange=255,128,0 | awk -v x="$(field "$A" 3)" -v y="$(field "$A" 4)" '$4 > 100 { print $2 - x, $3 - y }')"
	[[ -n "$ORANGE" ]] && break
	sleep 0.2
done
if [[ -n "$ORANGE" ]]; then
	n="$(hits a MENU orange)"
	expect "click the menu's orange item at $ORANGE (A-relative)" "$(vclick click "address:$A" $ORANGE)" ok
	until_ok 3 more a MENU orange "$n" && ok "the popup item activated" || bad "the popup item never activated"
else
	bad "the menu's orange item was never drawn"
fi


# --- 5. the glide -------------------------------------------------------------
echo "== the mouse mode: the user's pointer is on B; A is clicked and hovered"
mtimes() { # mtimes <client> <from-line>: the time of each wl_pointer.motion since
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.motion\([0-9]+' | grep -oE '[0-9]+$'
}
btime() { # btime <client> <from-line>: the time of the first wl_pointer.button since
	tail -n +"$(($2 + 1))" "$T/$1.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.button\([0-9]+, [0-9]+' | head -1 | grep -oE '[0-9]+$'
}
median_gap() { awk 'NR > 1 { print $1 - p } { p = $1 }' | sort -n | awk '{ g[NR] = $1 } END { print g[int((NR + 1) / 2)] }'; }
fitts() { python3 -c 'import math, sys; a, b, d, w = map(float, sys.argv[1:] + ["32"][len(sys.argv) - 4:]); print(round(min(max(a + b * math.log2(d / w + 1), 40), 1000)))' "$@"; } # fitts <a> <b> <d> [w]
dist() { python3 -c 'import math, sys; x, y, u, v = map(float, sys.argv[1:]); print(math.hypot(u - x, v - y))' "$@"; }
regex() { [[ "$1" =~ $2 ]]; }
rose() { # rose <png> <x> <y>: rose pixels (A's cursor image) in the 24x24 box from x-4,y-4 (its hotspot is its top-left)
	python3 -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
near = lambda p: abs(p[0] - 255) < 8 and p[1] < 8 and abs(p[2] - 128) < 8
print(sum(near(im.getpixel((i, j))) for i in range(max(x - 4, 0), min(x + 20, im.width)) for j in range(max(y - 4, 0), min(y + 20, im.height))))' "$@"
}
global() { read -r gx gy <<<"$(at "$1")" && echo "$(($(field "$A" 3) + gx)) $(($(field "$A" 4) + gy))"; } # global <color>: A's block on the screen
park "$BX" "$BY"
quiet "$T/a.wl" && quiet "$T/b.wl"
expect "the mouse mode" "$(vclick mode mouse)" mouse
POS="$(hq cursorpos)" ACTIVE="$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')"

AM="$(lines "$T/a.wl")"
expect "a jump-move to green: its surface coordinates" "$(vclick move "address:$A" $(at green) jump)" ok
quiet "$T/a.wl"
GREEN="$(last_xy a enter "$AM")" # the window box's point, plus GTK's shadow margin
expect "a known start: a jump-move to red, held" "$(vclick move "address:$A" $(at red) jump)" ok
quiet "$T/a.wl"
D="$(dist $(at red) $(at green))" MT="$(fitts 50 120 "$D")"
AM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")" n="$(hits a HIT green)"
expect "a glide click to green ($(printf '%.0f' "$D") px away)" "$(vclick click "address:$A" $(at green))" ok
expect "  status while it moves" "$(vclick status)" moving
expect "  anything else meanwhile is busy" "$(vclick click "address:$B" 10 10 | cut -c1-24)" "error: busy: still movin"
until_ok 4 idle || true
until_ok 3 more a HIT green "$n" && ok "  A hit green" || bad "  A never hit green"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^motion( motion){8,} button button$' && ok "  from the held hover: motions, then the click, and no leave ($(wc -w <<<"$EV") events)" || bad "  events: $EV"
M="$(mtimes a "$AM")"
GAP="$(head -n -1 <<<"$M" | median_gap)"
((GAP >= 6 && GAP <= 12)) && ok "  125 motions a second (median gap $GAP ms)" || bad "  median gap $GAP ms, want 6-12"
SPAN=$(($(tail -2 <<<"$M" | head -1) - $(head -1 <<<"$M")))
((SPAN >= MT - 10 && SPAN <= MT + 40)) && ok "  Fitts's time: $SPAN ms for $MT ms" || bad "  took $SPAN ms, want $MT ms (-10/+40)"
DWELL=$(($(btime a "$AM") - $(tail -2 <<<"$M" | head -1)))
((DWELL >= 50 && DWELL <= 130)) && ok "  a dwell on the target before the click ($DWELL ms)" || bad "  dwell $DWELL ms, want 50-130"
expect "  the last motion exactly on green" "$(last_xy a motion "$AM")" "$GREEN"
expect "B heard nothing" "$(events b "$BM")" ""
expect "the cursor never moved" "$(hq cursorpos)" "$POS"
expect "the focus never moved" "$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')" "$ACTIVE"
expect "A's own cursor image (set_cursor)" "$(vclick image "address:$A")" "the app's image, 16x16, hotspot 0,0"
expect "B's named shape (cursor-shape-v1): the theme's" "$(vclick image "address:$B")" "the theme's 'default', 20x20, hotspot 2,2"
D="$(dist $(at green) $(at red))" MT="$(fitts 50 120 "$D" 128)"
AM="$(lines "$T/a.wl")" n="$(hits a HIT red)"
expect "a glide click to red, named a 128 px target" "$(vclick click "address:$A" $(at red) size:128)" ok
until_ok 4 idle || true
until_ok 3 more a HIT red "$n" && ok "  A hit red" || bad "  A never hit red"
quiet "$T/a.wl"
M="$(mtimes a "$AM")"
SPAN=$(($(tail -2 <<<"$M" | head -1) - $(head -1 <<<"$M")))
((SPAN >= MT - 10 && SPAN <= MT + 40)) && ok "  Fitts's time with W 128: $SPAN ms for $MT ms" || bad "  took $SPAN ms, want $MT ms (-10/+40)"

echo "== the hover and the cursor stay where the input left them"
AM="$(lines "$T/a.wl")"
expect "move to blue" "$(vclick move "address:$A" $(at blue))" ok
until_ok 4 idle || true
read -r GX GY <<<"$(global blue)"
fullcap "$T/ghost.png" || bad "no capture with the cursor image"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^motion( motion){3,}$' && ok "  motions only: A's pointer was still there after the click" || bad "  events: $EV"
expect "  where" "$(vclick where)" "address:$A $(at blue)"
read -r _ CX CY CN <<<"$(python3 "$REPO/devtools/findcolors.py" "$T/ghost.png" rose=255,0,128)"
((CN >= 200 && CX >= GX + 5 && CX <= GX + 10 && CY >= GY + 5 && CY <= GY + 10)) && ok "  A's own image drawn at blue ($CN px at $CX,$CY; blue at $GX,$GY)" ||
	bad "  A's image: $CN px at $CX,$CY, want ~256 at $((GX + 7)),$((GY + 7))"
sleep 2 # past the tap mode's linger and fade
fullcap "$T/rest.png" || bad "no capture of the cursor at rest"
N="$(rose "$T/rest.png" "$GX" "$GY")"
((N >= 200)) && ok "  and it stays there, as a mouse at rest ($N px)" || bad "  gone after 2 s: $N px at blue"
AM="$(lines "$T/a.wl")" n="$(hits a HIT blue)"
expect "click where the hover is" "$(vclick click "address:$A" $(at blue))" ok
until_ok 4 idle || true
until_ok 3 more a HIT blue "$n" && ok "  A hit blue" || bad "  A never hit blue"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^motion( motion)* button button$' && ok "  no enter and no leave: the click came from the hover, which stays" || bad "  events: $EV"

read -r CGX CGY <<<"$(global cyan)"
dsp "hl.dsp.window.alter_zorder({ mode = 'top', window = 'address:$B' })" # the user's click in section 3 raised A
expect "a jump-move to cyan, under B" "$(vclick move "address:$A" $(at cyan) jump)" ok
quiet "$T/a.wl"
expect "  where" "$(vclick where)" "address:$A $(at cyan)"
fullcap "$T/under.png" || bad "no capture with the cursor under B"
expect "  B covers it, as it covers A" "$(rose "$T/under.png" "$CGX" "$CGY")" 0
dsp "hl.dsp.window.alter_zorder({ mode = 'top', window = 'address:$A' })"
fullcap "$T/over.png" || bad "no capture with A on top again"
N="$(rose "$T/over.png" "$CGX" "$CGY")"
((N >= 200)) && ok "  A on top again: drawn there ($N px)" || bad "  A on top again, not drawn: $N px"

AM="$(lines "$T/a.wl")"
expect "hide" "$(vclick hide)" ok
quiet "$T/a.wl"
expect "  A's pointer left" "$(events a "$AM")" leave
expect "  where" "$(vclick where)" nowhere
fullcap "$T/hidden.png" || bad "no capture after hide"
expect "  the cursor is gone" "$(rose "$T/hidden.png" "$GX" "$GY")" 0

AM="$(lines "$T/a.wl")" n="$(hits a HIT magenta)"
read -r MGX MGY <<<"$(global magenta)"
expect "a jump click on magenta" "$(vclick click "address:$A" $(at magenta) jump)" ok
until_ok 3 more a HIT magenta "$n" && ok "  A hit magenta" || bad "  A never hit magenta"
quiet "$T/a.wl"
expect "  an enter and the click, and no leave" "$(events a "$AM")" "enter motion button button"
expect "  where" "$(vclick where)" "address:$A $(at magenta)"
fullcap "$T/jump.png" || bad "no capture after the jump"
N="$(rose "$T/jump.png" "$MGX" "$MGY")"
((N >= 200)) && ok "  the cursor rests at magenta ($N px)" || bad "  no cursor at magenta: $N px"

echo "== the user's pointer in A takes the cursor; the next input starts where it left"
park "$((70 + RX))" "$((70 + RY))" # A's red, globally: the spot section 3 proved is A's
quiet "$T/a.wl"
expect "  where: the app's pointer is theirs" "$(vclick where)" nowhere
fullcap "$T/yield.png" || bad "no capture with the user's pointer in A"
expect "  not drawn meanwhile" "$(rose "$T/yield.png" "$MGX" "$MGY")" 0
park "$BX" "$BY"
expect "  where, after it left" "$(vclick where)" nowhere
fullcap "$T/back.png" || bad "no capture after the user's pointer left A"
expect "  not drawn back at its old spot" "$(rose "$T/back.png" "$MGX" "$MGY")" 0
# where the user's pointer left A, in A's coordinates, and as A's surface sees it (GREEN: the shadow margin)
EX=$((BX - $(field "$A" 3))) EY=$((BY - $(field "$A" 4)))
EX=$((EX < 0 ? 0 : EX >= $(field "$A" 5) ? $(field "$A" 5) - 1 : EX)) EY=$((EY < 0 ? 0 : EY >= $(field "$A" 6) ? $(field "$A" 6) - 1 : EY))
read -r GRX GRY <<<"$GREEN" && read -r gx gy <<<"$(at green)"
AM="$(lines "$T/a.wl")"
expect "move to yellow" "$(vclick move "address:$A" $(at yellow))" ok
until_ok 4 idle || true
quiet "$T/a.wl"
expect "  it entered where the user's pointer left A" "$(last_xy a enter "$AM")" "$((EX + GRX - gx)) $((EY + GRY - gy))"
AM="$(lines "$T/a.wl")"
park 1200 760 # the user's pointer leaves B for the empty desktop: the seat leaves every app's pointer
quiet "$T/a.wl"
expect "  the user's pointer changing surface ends the hover" "$(events a "$AM")" leave
park "$BX" "$BY"
expect "move to yellow again" "$(vclick move "address:$A" $(at yellow))" ok
until_ok 4 idle || true
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")" n="$(hits b HIT magenta)"
expect "  then click B (another app)" "$(vclick click "address:$B" $(at magenta) jump)" ok
until_ok 3 more b HIT magenta "$n" || bad "B never hit magenta"
quiet "$T/a.wl"
expect "  A's hover ended first" "$(events a "$AM")" leave
expect "  B's pointer is the user's: the cursor is theirs there" "$(vclick where)" nowhere

echo "== the user's pointer comes into A during a glide"
printf 'fitts_a_ms=400\nfitts_b_ms=400\novershoot_p=0\n' >"$T/slow.profile"
expect "a slow profile (1 s glides)" "$(vclick profile reload "$T/slow.profile" | head -1)" "loaded $T/slow.profile"
expect "  a jump-move to green" "$(vclick move "address:$A" $(at green) jump)" ok
quiet "$T/a.wl"
n="$(hits a HIT blue)"
expect "  a glide click to blue" "$(vclick click "address:$A" $(at blue))" ok
sleep 0.3
park "$((70 + RX))" "$((70 + RY))" # A's red, globally
POS="$(hq cursorpos)"
until_ok 4 idle || true
until_ok 3 more a HIT blue "$n" && ok "  A still hit blue" || bad "  A never hit blue"
expect "  the glide handed over to a jump" "$(vclick status)" "idle; the last glide finished as a jump: the user's pointer came into the app"
expect "  the cursor never moved" "$(hq cursorpos)" "$POS"
park "$BX" "$BY"
expect "back to the check's profile" "$(vclick profile reload "$T/even.profile" | head -1)" "loaded $T/even.profile"

echo "== the tap mode: a glide's cursor lingers and fades, and the pointer leaves"
expect "the tap mode" "$(vclick mode tap)" tap
expect "  nothing stays" "$(vclick where)" nowhere
read -r GGX GGY <<<"$(global green)"
AM="$(lines "$T/a.wl")" n="$(hits a HIT green)"
expect "a glide click to green" "$(vclick click "address:$A" $(at green) glide)" ok
until_ok 4 idle || true
fullcap "$T/linger.png" || bad "no capture as the glide lands"
until_ok 3 more a HIT green "$n" && ok "  A hit green" || bad "  A never hit green"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^enter motion( motion)* button button leave$' && ok "  an enter, motions, the click and the leave" || bad "  events: $EV"
N="$(rose "$T/linger.png" "$GGX" "$GGY")"
((N >= 200)) && ok "  the cursor drawn as it lands ($N px)" || bad "  no cursor as it lands: $N px"
sleep 2 # past the linger and the fade
fullcap "$T/faded.png" || bad "no capture after the fade"
expect "  then faded" "$(rose "$T/faded.png" "$GGX" "$GGY")" 0
expect "back to the mouse mode" "$(vclick mode mouse)" mouse

echo "== the cursor theme: B's named shapes, and its hidden cursor"
tint() { # tint <png> <x> <y> <r,g,b>: pixels of that color in the 36x36 box from x-12,y-12
	python3 -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
c = tuple(map(int, sys.argv[4].split(",")))
near = lambda p: all(abs(p[i] - c[i]) < 8 for i in range(3))
print(sum(near(im.getpixel((i, j))) for i in range(max(x - 12, 0), min(x + 24, im.width)) for j in range(max(y - 12, 0), min(y + 24, im.height))))' "$@"
}
bglobal() { read -r gx gy <<<"$(at "$1")" && echo "$(($(field "$B" 3) + gx)) $(($(field "$B" 4) + gy))"; } # B's blocks sit as A's
image_is() { [[ "$(vclick image "address:$1")" == "$2" ]]; }
park 1200 760 # the user's pointer on the empty desktop: no app's
dsp "hl.dsp.window.alter_zorder({ mode = 'top', window = 'address:$B' })"
read -r BMX BMY <<<"$(bglobal magenta)"
expect "a jump-move into B, over magenta (the window's shape)" "$(vclick move "address:$B" $(at magenta) jump)" ok
until_ok 3 image_is "$B" "the theme's 'default', 20x20, hotspot 2,2" || true
expect "  B's image: the theme's default" "$(vclick image "address:$B")" "the theme's 'default', 20x20, hotspot 2,2"
fullcap "$T/shape-default.png" || bad "no capture with the theme's default"
N="$(tint "$T/shape-default.png" "$BMX" "$BMY" 0,128,255)"
((N >= 360)) && ok "  drawn at magenta ($N px of 400)" || bad "  the theme's default at magenta: $N px of 400"
read -r BYX BYY <<<"$(bglobal yellow)"
expect "a move over yellow (text)" "$(vclick move "address:$B" $(at yellow) jump)" ok
until_ok 3 image_is "$B" "the theme's 'text', 20x20, hotspot 10,10" || true
expect "  B's image: the theme's text" "$(vclick image "address:$B")" "the theme's 'text', 20x20, hotspot 10,10"
fullcap "$T/shape-text.png" || bad "no capture with the theme's text"
N="$(tint "$T/shape-text.png" "$BYX" "$BYY" 128,255,0)"
((N >= 360)) && ok "  drawn centred on yellow ($N px of 400)" || bad "  the theme's text at yellow: $N px of 400"
read -r BRX BRY <<<"$(bglobal red)"
expect "a move over red (hidden)" "$(vclick move "address:$B" $(at red) jump)" ok
until_ok 3 image_is "$B" "nothing: the app hid its cursor" || true
expect "  B's image: nothing" "$(vclick image "address:$B")" "nothing: the app hid its cursor"
expect "  where: the mouse is there all the same" "$(vclick where)" "address:$B $(at red)"
fullcap "$T/shape-hidden.png" || bad "no capture with the cursor hidden"
expect "  nothing drawn at red" "$(tint "$T/shape-hidden.png" "$BRX" "$BRY" 0,128,255) $(tint "$T/shape-hidden.png" "$BRX" "$BRY" 128,255,0)" "0 0"
expect "hide" "$(vclick hide)" ok
dsp "hl.dsp.window.alter_zorder({ mode = 'top', window = 'address:$A' })"
park "$BX" "$BY"

echo "== a held button: press, a held move, release"
quiet "$T/a.wl"
expect "release with nothing held" "$(vclick release)" "error: no button is held"
expect "lua: so too" "$(lua 'hl.plugin.vclick.release()')" $'nil\terror: no button is held'
AM="$(lines "$T/a.wl")" GL="$(lines "$T/a.log")" n="$(hits a HIT red)"
expect "press on red (a jump)" "$(vclick press "address:$A" $(at red) jump)" ok
until_ok 3 more a HIT red "$n" && ok "  A heard the press on red" || bad "  A never heard the press"
expect "  status: holding it" "$(vclick status)" "idle; holding left"
expect "  a click meanwhile: refused" "$(vclick click "address:$A" $(at green))" "error: busy: holding left; release it first"
expect "  a move into B: refused" "$(vclick move "address:$B" 10 10)" "error: busy: holding left in another window; release it first"
expect "a held move to green (a glide)" "$(vclick move "address:$A" $(at green))" ok
expect "  status while it moves" "$(vclick status)" dragging
until_ok 4 idle || true
expect "  where" "$(vclick where)" "address:$A $(at green)"
expect "release" "$(vclick release)" ok
let_go() { tail -n +"$((GL + 1))" "$T/a.log" | grep -q '^UP '; }
until_ok 3 let_go || true
read -r _ UPW UPX _ <<<"$(tail -n +"$((GL + 1))" "$T/a.log" | grep '^UP ' | tail -1)"
[[ "$UPW" == red ]] && ((UPX >= 120)) && ok "  GTK: up in red, the press's block, at green past its edge (x $UPX)" || bad "  GTK's release: $UPW at x $UPX, want red at x >= 120"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^enter motion button( motion){3,} button$' && ok "  an enter, the press, motions, the release, and no leave between ($(wc -w <<<"$EV") events)" || bad "  events: $EV"
expect "  the release where green is (A's surface)" "$(last_xy a motion "$AM")" "$GREEN"
expect "  status: idle" "$(vclick status)" idle

echo "== a held button never moves the window: the app's move request is refused"
read -r BCX BCY <<<"$(bglobal cyan)" && read -r BGX BGY <<<"$(bglobal magenta)" # inside B, clear of A
park "$BCX" "$BCY"
# devtools/fixwin with drag: its own title bar, asking xdg_toplevel.move with
# the press's serial once dragged 8 px (as GTK's does, which reads its
# motion only on its frame clock)
MT="vclick-move-$$"
dsp "hl.dsp.exec_cmd('$REPO/devtools/fixwin 300 200 $MT drag > $T/fix.log 2>&1')"
fixwin() { hq clients -j | python3 -c 'import json, sys
for c in json.load(sys.stdin):
    if c["class"] == sys.argv[1]: print(c["address"], *c["at"], int(c["floating"]))' "$MT"; }
fix_up() { [[ -n "$(fixwin)" ]]; }
until_ok 5 fix_up || bad "the fixture never mapped"
read -r F _ _ FL <<<"$(fixwin)"
[[ "$FL" == 1 ]] || dsp "hl.dsp.window.float({ action = 'enable', window = 'address:$F' })"
dsp "hl.dsp.window.move({ x = 700, y = 420, window = 'address:$F' })"
fix_at() { [[ "$(fixwin | cut -d' ' -f2-3)" == "$1 $2" ]]; }
until_ok 3 fix_at 700 420 || bad "the fixture never moved to 700,420"
park "$BGX" "$BGY" && park "$BCX" "$BCY" # it mapped under the user's pointer: back on B
expect "press on the fixture's title bar" "$(vclick press "address:$F" 50 50 jump)" ok
expect "  a held move 100 px right" "$(vclick move "address:$F" 150 90 jump)" ok
asked() { grep -q '^move ' "$T/fix.log"; }
until_ok 3 asked && ok "  it asked to be moved" || bad "  it never asked to be moved"
expect "  with the press's serial" "$(awk '$1 == "move" { print $2 }' "$T/fix.log")" "$(awk '$1 == "press" { print $2 }' "$T/fix.log" | tail -1)"
park "$BGX" "$BGY" # the user's mouse moves (within B): a move it drove would carry the window along
expect "release" "$(vclick release)" ok
expect "  the window never moved" "$(fixwin | cut -d' ' -f2-3)" "700 420"
pkill -f "fixwin 300 200 $MT"
fix_gone() { [[ -z "$(fixwin)" ]]; }
until_ok 3 fix_gone || bad "the fixture never closed"

echo "== a drag-and-drop the app begins from vclick's button is refused"
quiet "$T/a.wl" && quiet "$T/b.wl"
GL="$(lines "$T/a.log")" TM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")"
read -r YX YY <<<"$(at yellow)"
expect "press on yellow (a drag source)" "$(vclick press "address:$A" "$YX" "$YY" jump)" ok
expect "  a held move 80 px on" "$(vclick move "address:$A" $((YX + 80)) "$YY" jump)" ok
dnd_over() { tail -n +"$((GL + 1))" "$T/a.log" | grep -q '^DRAGEND yellow'; }
until_ok 3 dnd_over || true
expect "  A began one, and it failed" "$(tail -n +"$((GL + 1))" "$T/a.log" | grep -oE '^DRAG(BEGIN|FAILED|END) yellow' | tr '\n' ' ' | sed 's/ $//')" "DRAGBEGIN yellow DRAGFAILED yellow DRAGEND yellow"
expect "  its source cancelled" "$(tail -n +"$((TM + 1))" "$T/a.wl" | grep -v ' -> ' | grep -c 'wl_data_source#[0-9]*\.cancelled(')" 1
expect "  the hold ended, and says why" "$(vclick status)" "idle; the hold ended: the app began a drag-and-drop, which only the user's mouse can drive"
expect "  nothing held" "$(vclick release)" "error: no button is held"
park "$BCX" "$BCY"
n="$(hits b HIT cyan)"
ptr "press 272" "sleep 30" "release 272" # the user's own click, on B: no drop lands there
until_ok 3 more b HIT cyan "$n" && ok "  the user's click on B is a click" || bad "  the user's click on B went nowhere"
quiet "$T/b.wl"
expect "  and B was offered no drop" "$(tail -n +"$((BM + 1))" "$T/b.wl" | grep -v ' -> ' | grep -cE 'wl_data_device#[0-9]+\.(enter|drop)\(')" 0

echo "== the user's pointer changing surface ends a hold (the seat leaves every app's pointer)"
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")"
expect "press on red" "$(vclick press "address:$A" $(at red) jump)" ok
park 1200 760 # from B to the empty desktop
expect "  the hold ended, and says why" "$(vclick status)" "idle; the hold ended: the user's pointer changed surface, and the seat let the button go"
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^(enter )?motion button button leave$' && ok "  A heard the release, then the leave, from the seat" || bad "  events: $EV"
expect "  nothing held" "$(vclick release)" "error: no button is held"
park "$BX" "$BY"

echo "== a held button in the tap mode; hide lets it go"
expect "the tap mode" "$(vclick mode tap)" tap
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")"
expect "press on blue" "$(vclick press "address:$A" $(at blue) jump)" ok
expect "  where: shown while it is held" "$(vclick where)" "address:$A $(at blue)"
expect "  release" "$(vclick release)" ok
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^(leave )?enter motion button button leave$' && ok "  the press, the release, then the leave" || bad "  events: $EV"
expect "back to the mouse mode" "$(vclick mode mouse)" mouse
AM="$(lines "$T/a.wl")"
expect "press on blue" "$(vclick press "address:$A" $(at blue) jump)" ok
expect "  hide: the button goes up, the pointer leaves" "$(vclick hide)" ok
quiet "$T/a.wl"
EV="$(events a "$AM")"
regex "$EV" '^(enter )?motion button button leave$' && ok "  the release, then the leave" || bad "  events: $EV"
expect "  nothing held" "$(vclick status)" idle

# --- 6. under the lockscreen ---------------------------------------------------
echo "== the lockscreen (devtools/vlock): vclick's input reaches A, the user's the lockscreen"
mkfifo "$T/vl.fifo"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/vlock" <"$T/vl.fifo" >"$T/vl.log" 2>&1 &
exec 5>"$T/vl.fifo"
locked_is() { [[ "$(hq locked)" == "$1" ]]; }
vl() { grep -c "^$1\$" "$T/vl.log"; } # vl <line>: how often the lock surfaces heard it
vl_more() { (($(vl "$1") > $2)); }
expect "a slow profile" "$(vclick profile reload "$T/slow.profile" | head -1)" "loaded $T/slow.profile"
expect "a glide is going" "$(vclick move "address:$A" $(at green))" ok
echo lock >&5
until_ok 5 locked_is true || bad "the nested never locked"
until_ok 4 idle || true
expect "  the lock didn't stop it" "$(vclick status)" idle
expect "  it ended on green" "$(vclick where)" "address:$A $(at green)"
expect "back to the check's profile" "$(vclick profile reload "$T/even.profile" | head -1)" "loaded $T/even.profile"
KL="$(vl kleave)" PL="$(vl pleave)"
n="$(hits a HIT red)"
expect "a jump click on red" "$(vclick click "address:$A" $(at red) jump)" ok
until_ok 3 more a HIT red "$n" && ok "  A hit red" || bad "  A never hit red"
n="$(scrolls a magenta down)"
expect "a scroll over magenta" "$(vclick scroll "address:$A" $(at magenta) down 2)" ok
scroll_ok a magenta down "$n" 2
AM="$(lines "$T/a.wl")" n="$(keys a Page_Down -)"
expect "a key" "$(vclick key "address:$A" Next)" ok
until_ok 3 keyed a Page_Down - "$n" && ok "  A read Page_Down" || bad "  A never read Page_Down"
quiet "$T/a.wl"
expect "  as an enter, the key and a leave: the lockscreen has the user's keyboard" "$(kevents a "$AM")" "enter modifiers key key modifiers leave"
n="$(hits a HIT green)"
expect "a glide click on green (the mouse mode)" "$(vclick click "address:$A" $(at green))" ok
until_ok 4 idle || true
until_ok 3 more a HIT green "$n" && ok "  A hit green" || bad "  A never hit green"
expect "  it went the whole way" "$(vclick status)" idle
read -r GGX GGY <<<"$(global green)"
fullcap "$T/lockcur.png" || bad "no capture under the lockscreen"
expect "  no cursor drawn over the lockscreen" "$(rose "$T/lockcur.png" "$GGX" "$GGY")" 0
expect "the lock surface kept the keyboard and the pointer" "$(vl kleave) $(vl pleave)" "$KL $PL"
n="$(keys a a -)" k="$(vl 'key 30')"
kb "tap 30"
until_ok 3 vl_more 'key 30' "$k" && ok "the user's own key goes to the lockscreen" || bad "the lockscreen never heard the user's key"
quiet "$T/a.wl"
expect "  and A never read it" "$(keys a a -)" "$n"
read -r RGX RGY <<<"$(global red)"
park "$RGX" "$RGY"
n="$(hits a HIT red)" b="$(vl 'button 272')"
ptr "press 272" "sleep 30" "release 272"
until_ok 3 vl_more 'button 272' "$b" && ok "the user's own click over A goes to the lockscreen" || bad "the lockscreen never heard the user's click"
quiet "$T/a.wl"
expect "  and A never heard it" "$(hits a HIT red)" "$n"
park "$BX" "$BY"
wincap "$(field "$A" 2)" "$T/locked.png" && ok "a window capture of A works under the lockscreen" || bad "no window capture of A under the lockscreen"
expect "  A as it is drawn: red where it was" "$(python3 "$REPO/devtools/findcolors.py" "$T/locked.png" red=255,0,0 | awk '{ print $2, $3 }')" "$(at red)"
echo unlock >&5
until_ok 5 locked_is false || bad "the nested never unlocked"
exec 5>&-
expect "unlocked: a click goes as ever" "$(vclick click "address:$A" $(at red) jump)" ok
park "$BX" "$BY"

# --- 7. unload ----------------------------------------------------------------
echo "== unload during a glide"
quiet "$T/a.wl"
expect "a slow profile" "$(vclick profile reload "$T/slow.profile" | head -1)" "loaded $T/slow.profile"
AM="$(lines "$T/a.wl")"
expect "a glide is going" "$(vclick move "address:$A" $(at green))" ok
sleep 0.3
hq plugin unload "$SO" >/dev/null 2>&1
quiet "$T/a.wl"
E1="$(events a "$AM")"
sleep 0.5
expect "  it ended with the plugin: no pointer event after" "$(events a "$AM")" "$E1"
expect "  and left A's pointer" "$(events a "$AM" | awk '{ print $NF }')" leave
expect "  the nested is alive" "$(kill -0 "$NPID" 2>/dev/null && echo yes)" yes
hq plugin load "$SO" >/dev/null 2>&1
expect "loaded again" "$(hq plugin list | grep -c 'Plugin vclick')" 1

echo "== unload while a button is held"
quiet "$T/a.wl"
AM="$(lines "$T/a.wl")"
expect "press on red" "$(vclick press "address:$A" $(at red) jump)" ok
hq plugin unload "$SO" >/dev/null 2>&1
quiet "$T/a.wl"
expect "  the button went up, then A's pointer left" "$(events a "$AM")" "enter motion button button leave"
hq plugin load "$SO" >/dev/null 2>&1
expect "loaded again" "$(hq plugin list | grep -c 'Plugin vclick')" 1

echo "== unload during a paced scroll"
n="$(scrolls a magenta down)"
expect "a run is going" "$(vclick scroll "address:$A" $(at magenta) down 20 100ms jump)" ok
hq plugin unload "$SO" >/dev/null 2>&1
sleep 2.5
quiet "$T/a.wl"
s=$(($(scrolls a magenta down) - n))
((s >= 1 && s < 20)) && ok "  it ended with the plugin ($s of 20 steps)" || bad "  $s of 20 steps after the unload"
expect "unloaded" "$(hq plugin list | grep -c 'Plugin vclick')" 0
expect "the command is gone" "$(vclick click "address:$A" 10 10 | grep -c '^ok$')" 0
expect "so are its Lua functions" "$(lua 'hl.plugin.vclick.status()' | cut -c1-6)" "error:"
expect "the nested is alive" "$(kill -0 "$NPID" 2>/dev/null && echo yes)" yes

echo
if ((FAIL == 0)); then
	echo "== vclick: ALL $PASS CHECKS PASSED =="
else
	echo "== vclick: $FAIL of $((PASS + FAIL)) CHECKS FAILED == (artifacts: $T)"
	exit 1
fi
