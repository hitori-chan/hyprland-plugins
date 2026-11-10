#!/usr/bin/env bash
# vclick's nested check: a click lands where a window capture shows it,
# under a covering window, in a popup, a scroll steps exactly, and the
# user's pointer never notices. Runs a nested Hyprland in the gate's private host
# (awesome/gate/launch.sh) with only vclick loaded; the live session is
# never touched.
#
# usage: make -C vclick && bash vclick/check.sh
# Two GTK3 clients (devtools/gtkgrid.py, each with a protocol trace):
# A is the target, B covers A's lower right and is the bystander. A
# virtual pointer (devtools/vptr) is "the user's" mouse.
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

MON_W=1280 MON_H=800
cat >"$T/nested.lua" <<EOF
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
	exec 7>&- 2>/dev/null
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

# --- the clients ----------------------------------------------------------
grids() { # address stableId at.x at.y size.x size.y floating, per gtkgrid
	hq clients -j | python3 -c '
import json, sys
for c in json.load(sys.stdin):
    if c["title"] == "gtkgrid":
        print(c["address"], c["stableId"], *c["at"], *c["size"], int(c["floating"]))'
}
open_grid() { # open_grid <name>: spawn, wait for READY and the window; prints its address
	local before
	before="$(grids | cut -d' ' -f1)"
	dsp "hl.dsp.exec_cmd('python3 $REPO/devtools/gtkgrid.py $T/$1.log wayland $T/$1.wl')"
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

A="$(open_grid a)" || { echo "check: client A never mapped" >&2; exit 1; }
B="$(open_grid b)" || { echo "check: client B never mapped" >&2; exit 1; }
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
expect "unknown window" "$(vclick address:0x1 10 10)" "error: window not found"
expect "outside the window" "$(vclick "address:$A" 5000 10 | sed 's/ (.*//')" "error: 5000,10 is outside the window"
expect "unknown button" "$(vclick "address:$A" 10 10 thumb | head -1)" "error: unknown button 'thumb'"
expect "count bound" "$(vclick "address:$A" 10 10 left 4 | head -1)" "error: count must be 1-3"
expect "steps bound" "$(vclick "address:$A" 10 10 scroll-down 21 | head -1)" "error: steps must be 1-20"

# --- 2. another app under the user's pointer --------------------------------
echo "== the user's pointer is on B (another client); A is clicked"
BX=$(($(field "$B" 3) + $(field "$B" 5) / 2)) BY=$(($(field "$B" 4) + $(field "$B" 6) / 2))
park "$BX" "$BY"
quiet "$T/a.wl" && quiet "$T/b.wl"
POS="$(hq cursorpos)" ACTIVE="$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')"
AM="$(lines "$T/a.wl")" BM="$(lines "$T/b.wl")"
for c in red cyan; do # opposite corners: an offset or a scale error misses one; cyan is under B
	n="$(hits a HIT "$c")"
	expect "click $c at $(at "$c")" "$(vclick "address:$A" $(at "$c"))" ok
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
expect "scroll down 3 steps over magenta" "$(vclick "address:$A" $(at magenta) scroll-down 3)" ok
scroll_ok a magenta down "$n" 3
n="$(scrolls a cyan up)"
expect "scroll up 2 steps over cyan (under B)" "$(vclick "address:$A" $(at cyan) scroll-up 2)" ok
scroll_ok a cyan up "$n" 2
# GTK3 (3.24.52) names a +x wheel step "left", a real wheel's too: the
# horizontal step is held against the user's own wheel in the next section,
# not against GTK's name
GL="$(lines "$T/a.log")"
expect "scroll right over green" "$(vclick "address:$A" $(at green) scroll-right)" ok
until_ok 3 has_new_scroll a "$GL" green || true
quiet "$T/a.wl"
VDIR="$(new_scrolls a "$GL" green)"
expect "  a scrolled green 1 step, horizontally" "$(sed 's/right/x/; s/left/x/' <<<"$VDIR")" x
quiet "$T/b.wl"
expect "A heard one enter, the steps, one leave, per scroll" "$(events a "$AM")" "enter motion axis axis axis leave enter motion axis axis leave enter motion axis leave"
expect "  each step one detent (axis:value120)" "$(detents a "$AM")" "0:120 0:120 0:120 0:-120 0:-120 1:120"
expect "  each step 15 units (axis:value)" "$(axes a "$AM")" "0:15 0:15 0:15 0:-15 0:-15 1:15"
expect "B heard nothing" "$(events b "$BM")" ""
expect "the cursor never moved" "$(hq cursorpos)" "$POS"
expect "the focus never moved" "$(hq activewindow -j | python3 -c 'import json,sys; print(json.load(sys.stdin).get("address"))')" "$ACTIVE"

AM="$(lines "$T/a.wl")" n="$(hits a HIT green)"
expect "a double click (count 2)" "$(vclick "address:$A" $(at green) left 2)" ok
until_ok 3 atleast a HIT green $((n + 2)) && ok "A saw both presses" || bad "A saw $(($(hits a HIT green) - n)) presses"
quiet "$T/a.wl"
expect "  as one enter, two clicks, one leave" "$(events a "$AM")" "enter motion button button button button leave"
AM="$(lines "$T/a.wl")"
expect "a right click" "$(vclick "address:$A" $(at blue) right)" ok
quiet "$T/a.wl"
expect "  is button 273" "$(tail -n +"$((AM + 1))" "$T/a.wl" | grep -v ' -> ' | grep -oE 'wl_pointer[#@][0-9]+\.button\([0-9]+, [0-9]+, [0-9]+' | awk -F', ' '{ print $3 }' | sort -u)" 273

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
expect "click yellow" "$(vclick "address:$A" $(at yellow))" ok
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
expect "scroll down over yellow" "$(vclick "address:$A" $(at yellow) scroll-down)" ok
scroll_ok a yellow down "$n" 1
expect "  A's pointer stepped out and back" "$(events a "$AM" | sed 's/ motion$//')" "leave enter motion axis leave enter"
expect "  back in under the cursor" "$(last_xy a enter "$AM")" "$(fmt_xy $((LX - 10)) $((LY - 10)))"
expect "the cursor never moved" "$(hq cursorpos)" "$POS"

echo "== the user holds a button in A"
quiet "$T/a.wl"
n="$(hits a HIT red)"
ptr "press 272"
until_ok 3 more a HIT red "$n" || bad "the user's press never reached A"
expect "A is busy while the user's button is down" "$(vclick "address:$A" $(at yellow) | cut -c1-11)" "error: busy"
n="$(hits b HIT magenta)"
read -r MX MY <<<"$(at magenta)" # B is the same app as A: its blocks sit alike
expect "B (another app) still clicks" "$(vclick "address:$B" "$MX" "$MY")" ok
until_ok 3 more b HIT magenta "$n" && ok "B hit magenta" || bad "B never hit magenta"
ptr "release 272"

# --- 4. a popup -------------------------------------------------------------
echo "== a popup: A's menu"
n="$(hits a MENUBUTTON True)"
expect "click the menu button" "$(vclick "address:$A" $(at button))" ok
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
	expect "click the menu's orange item at $ORANGE (A-relative)" "$(vclick "address:$A" $ORANGE)" ok
	until_ok 3 more a MENU orange "$n" && ok "the popup item activated" || bad "the popup item never activated"
else
	bad "the menu's orange item was never drawn"
fi

# --- 5. unload ----------------------------------------------------------------
echo "== unload"
hq plugin unload "$SO" >/dev/null 2>&1
expect "unloaded" "$(hq plugin list | grep -c 'Plugin vclick')" 0
expect "the command is gone" "$(vclick "address:$A" 10 10 | grep -c '^ok$')" 0
expect "the nested is alive" "$(kill -0 "$NPID" 2>/dev/null && echo yes)" yes

echo
if ((FAIL == 0)); then
	echo "== vclick: ALL $PASS CHECKS PASSED =="
else
	echo "== vclick: $FAIL of $((PASS + FAIL)) CHECKS FAILED == (artifacts: $T)"
	exit 1
fi
