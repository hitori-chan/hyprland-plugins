# awesome/gate/windows.sh — the windows module's behavior battery: spawn
# placement, the CSD geometry battery (a real GTK3 client included), and
# maximize/minimize/restore round-trips. A geometry change runs this file
# alone (-b windows).
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh" # until_is

# ---- placement memory ---------------------------------------------------
# The harness seeds foot's spot at (100,100). Memory is the POSITION; the
# size is always the client's own request.
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; settled foot 1
expect "spawn memory: the requested 600x300 lands at the remembered (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[600,300] for c in cs)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; settled foot 2
expect "a sibling keeps its own requested 600x300 too" \
	"sum(1 for c in cs if c['class']=='foot' and c['size']==[600,300])==2"
expect "sibling lands off the taken spot — no exact stacking" \
	"len(set(tuple(c['at']) for c in cs if c['class']=='foot'))==2"
B="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at'] != [100,100]), ''))")"
A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at']==[100,100]), ''))")"
dsp "hl.dsp.window.close({window=\"address:$A\"})"
for _ in $(seq 1 30); do
	LEFT="$(clients | python3 -c "
import json,sys
address = sys.argv[1]
print(any(c['address'] == address for c in json.load(sys.stdin)))" "$A")"
	[[ "$LEFT" != 1 ]] && break
	sleep 0.1
done
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; settled foot 2
placementReclaim() {
	clients | python3 -c "
import json,sys
cs = json.load(sys.stdin)
b = next((c for c in cs if c['address'] == '$B'), None)
def overlaps(c):
    return c['at'][0] < 700 and c['at'][0] + c['size'][0] > 100 and c['at'][1] < 400 and c['at'][1] + c['size'][1] > 100
on_spot = any(c['class'] == 'foot' and c['at'] == [100,100] and c['size'] == [600,300] for c in cs)
positions = {tuple(c['at']) for c in cs if c['class'] == 'foot'}
print(1 if b and len(positions) == 2 and ((not overlaps(b) and on_spot) or (overlaps(b) and not on_spot)) else 0)"
}
chk "freed spot reuse respects remaining sibling geometry" test "$(placementReclaim)" = 1

# fullscreen roundtrip on the focused (newest) foot
feet() { clients | python3 -c "
import json,sys
print(sorted((c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot'))"; }
FEET="$(feet)"
dsp "hl.dsp.window.fullscreen()"; settled foot 2; dsp "hl.dsp.window.fullscreen()"; settled foot 2
chk "fullscreen roundtrip restores the exact boxes" test "$(feet)" = "$FEET"

# ---- close storm + memory update ---------------------------------------
storm_jobs=()
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" & storm_jobs+=("$!")
done; [[ ${#storm_jobs[@]} -gt 0 ]] && wait "${storm_jobs[@]}" || true
chk "close storm: no stragglers" until_is 5 0 pyc "sum(1 for c in cs if c['class']=='foot')"
# kind-exact: a maximize in an earlier battery (quick) leaves a windowed
# restore row for the same class — the coalesced-save invariant is about
# the SPOT row
chk "tsv: exactly one foot spot row survives the coalesced save" \
	until_is 3 1 grep -c $'^spot\t-*[0-9]*\t-*[0-9]*\tfoot$' "$AW_STATE"
chk "tsv: no temp-file debris" bash -c "! ls $AWSTATE/*.tmp 2>/dev/null | grep -q ."

# ---- fixed-size (dialog/splash) placement --------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=700x500')"; settled foot 1
dsp "hl.dsp.exec_cmd('$REPO/devtools/fixwin 310 360')"; settled fixwin 1
expect "fixed-size dialog keeps the centered native spot, not the least-overlap corner" \
	"any(abs(c['at'][0]-$(( (MON_W-310)/2 )))<=14 and abs(c['at'][1]-$(( 26+(MON_H-26-360)/2 )))<=14 for c in cs if c['class']=='fixwin')"
FW="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='fixwin'), ''))")"
close_wait "$FW"; sleep 0.5 # the store's save of the close, had it written one
chk "fixed-size dialog never writes the class row" \
	bash -c "! grep -q $'\tfixwin\$' \"$AW_STATE\""
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
close_wait "$FF"
chk "fixed-size battery left no windows" \
	test "$(pyc "sum(1 for c in cs if c['class'] in ('fixwin','foot'))")" = 0

# ---- CSD geometry offset (discord-updater splash shape) ------------------
# The window box is the CONTENT frame: the client declares its content
# rectangle inside a bigger buffer (the CSD shadow margin), and the
# compositor shows exactly that rectangle at the box — the margin is
# cropped, never padding inside the box nor a halo outside it. No gap
# between the border and the content, on any side; the size round-trips in
# the content frame.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 csdpin')"; settled csdpin
expect "pinned CSD splash: box is the content frame (300x350), centered" \
	"any(c['class']=='csdpin' and c['floating'] and c['size']==[300,350] and abs(c['at'][0]-$(( (MON_W-300)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-350)/2 )))<=14 for c in cs)"
box4() { clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class']=='$1'), None)
print(f\"{c['at'][0]} {c['at'][1]} {c['size'][0]} {c['size'][1]}\") if c else print('none')" ; }
capture_nested "$STATE/csd-pin.png"
chk "pinned CSD: content fills the box, the margin is cropped" \
	test "$(python3 - "$STATE/csd-pin.png" "$(box4 csdpin)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
x, y, w, h = map(int, sys.argv[2].split())
MAROON = (48, 32, 32)
# 3px in from the box edge: content, at every corner and the center (the
# 1px border sits on the box edge itself)
content = lambda dx, dy: px[x + dx, y + dy] == MAROON
ok = content(3, 3) and content(w - 4, 3) and content(3, h - 4) and content(w - 4, h - 4) \
   and content(w // 2, h // 2)
# 5px OUTSIDE the box edge: never content
outside = lambda dx, dy: px[dx, dy] != MAROON
ok = ok and outside(x - 5, y + h // 2) and outside(x + w + 5, y + h // 2) \
      and outside(x + w // 2, y - 5) and outside(x + w // 2, y + h + 5)
print(1 if ok else 0)
PY
)" = 1
CP="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdpin'), ''))")"
close_wait "$CP"
# Input follows the content frame: the client's surface coords start at
# the margin's outer corner, so a pointer at box-local (x, y) lands at
# (x + m, y + m) in the client. (A GTK file chooser got every click shifted
# up-left by its shadow margin when only the drawing cropped it.)
PTRLOG="$STATE/csdptr.log"; : >"$PTRLOG"
dsp "hl.dsp.exec_cmd('env SPLASHWIN_POINTER_LOG=$PTRLOG $REPO/devtools/splashwin 300 200 20 csdptr - - - vismargin')"; settled csdptr
read -r PX PY _ _ <<<"$(box4 csdptr)"
if [[ "$PX" =~ ^[0-9]+$ ]]; then
	# one device for the whole gesture: the client binds wl_pointer on the
	# capability gain, then sees the real enter/motion
	csd_ptr() { # csd_ptr <box-dx> <box-dy> <want "at X Y"> <name>
		printf 'sleep 300\nmove %s %s\nsleep 150\n' "$((PX + $1))" "$((PY + $2))" | vp
		local got; got="$(tail -1 "$PTRLOG")"
		[[ "$got" == "$3" ]] && ok "$4" || bad "$4 (client saw '$got')"
	}
	csd_ptr 3 3 "at 23 23" "CSD input: the content frame's corner lands at the margin (23,23)"
	csd_ptr 150 100 "at 170 120" "CSD input: the content center lands at box-local + margin (170,120)"
	# a pointer device appearing while the cursor rests over the window (a
	# re-added device after resume): the client binds a new wl_pointer and
	# must be entered where the cursor is, not at (-1,-1) until it moves
	printf 'sleep 300\n' | vp
	GOT="$(tail -1 "$PTRLOG")"
	[[ "$GOT" == "at 170 120" ]] && ok "a late-bound pointer enters where the cursor rests (170,120)" ||
		bad "a late-bound pointer enters where the cursor rests (170,120) (client saw '$GOT')"
else
	bad "CSD input: the csdptr window mapped"
fi
CQ="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdptr'), ''))")"
close_wait "$CQ"

# a FOLLOWING CSD client (real GTK shape): it resizes its content to the
# configure, so any frame mismatch shows up as a clipped or shrunk content.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfollow - - resz vismargin follow')"; settled csdfollow
expect "following CSD: box is the content frame (800x500), centered" \
	"any(c['class']=='csdfollow' and c['floating'] and c['size']==[800,500] and abs(c['at'][0]-$(( (MON_W-800)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-500)/2 )))<=14 for c in cs)"
capture_nested "$STATE/csd-follow.png"
chk "following CSD: content whole to the box edge, margin cropped" \
	test "$(python3 - "$STATE/csd-follow.png" "$(box4 csdfollow)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
x, y, w, h = map(int, sys.argv[2].split())
# the client resized its content to the configure: content reaches every
# box edge (not clipped, not shrunken), and the vismargin gray the client
# paints in its buffer margin is never drawn — not inside the box, not
# outside it.
content = lambda dx, dy: px[x + dx, y + dy] == (48, 32, 32)
margin  = lambda dx, dy: px[dx, dy] == (176, 176, 176)
ok = content(3, 3) and content(w - 4, 3) and content(3, h - 4) and content(w - 4, h - 4) \
   and content(w // 2, h // 2) \
   and not margin(x - 5, y + h // 2) and not margin(x + w + 5, y + h // 2) \
   and not margin(x + w // 2, y - 5) and not margin(x + w // 2, y + h + 5)
print(1 if ok else 0)
PY
)" = 1
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfollow'), ''))")"
close_wait "$CF"
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfollow - - resz vismargin follow')"; settled csdfollow
expect "following CSD: the remembered spot round-trips at the content-frame size" \
	"any(c['class']=='csdfollow' and c['size']==[800,500] and c['at']==[$(( (MON_W-800)/2 )), $(( 30+(MON_H-30-500)/2 ))] for c in cs)"
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfollow'), ''))")"
close_wait "$CF"

# a close in maximized state must mint the spot from the app's last
# windowed box (a browser that always closes maximized keeps its memory);
# the fresh class has only ever been maximized, so a spot row proves the
# fallback ran.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfall - - resz vismargin follow')"; settled csdfall
dsp "hl.plugin.awesome.maximize()"; settled csdfall
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfall'), ''))")"
close_wait "$CF"
chk "maximized close mints the spot from the last windowed box" \
	until_ok 3 grep -q "^spot	[0-9]*	[0-9]*	csdfall$" "$AW_STATE"
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 800 500 20 csdfall - - resz vismargin follow')"; settled csdfall
expect "minted spot lands the respawn at the remembered box" \
	"any(c['class']=='csdfall' and c['size']==[800,500] and c['at']==[$(( (MON_W-800)/2 )), $(( 30+(MON_H-30-500)/2 ))] for c in cs)"
CF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdfall'), ''))")"
close_wait "$CF"
# A small window of a class remembered big keeps its own size: the spot
# row carries a 700x500 close-box, the respawn asks for 300x200 and gets
# it, at the remembered position (2026-10-05: the portal file chooser and
# bleachbit's dialogs were born at the app's remembered main-window size).
csdsmall_addr() { clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdsmall'), ''))"; }
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 700 500 20 csdsmall - - resz - - - - follow')"; settled csdsmall
dsp "hl.dsp.window.move({x = 140, y = 120, window = \"address:$(csdsmall_addr)\"})"; settled csdsmall
close_wait "$(csdsmall_addr)"
until_ok 3 grep -q $'^spot\t140\t120\tcsdsmall$' "$AW_STATE"
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 200 20 csdsmall - - resz - - - - follow')"; settled csdsmall
expect "a small window of a class remembered 700x500 keeps its own 300x200, at the remembered spot" \
	"any(c['class']=='csdsmall' and c['at']==[140,120] and c['size']==[300,200] for c in cs)"
close_wait "$(csdsmall_addr)"
# ---- a real GTK3 client: what is drawn is what is clicked ----------------
# Render-vs-input with a real toolkit: find where each color block is DRAWN
# (a capture), click its center, and the client must name that block — the
# floating CSD window (its content offset), its GtkMenu (an xdg_popup placed
# against the window geometry), and the same app over XWayland. Then the
# positioner: a menu that fits below its button near the bottom edge opens
# below it, not flipped (2026-10-05: the popup pass drew menus a shadow
# margin up-left of where input put them — a click on an item hit the one
# above — and the solver measured the parent from box + margin).
if python3 -c 'import gi; gi.require_version("Gtk", "3.0"); gi.require_version("Gdk", "3.0"); from gi.repository import Gdk, Gtk' 2>/dev/null; then
	GG_LOG="$STATE/gtkgrid.log" GG_WL="$STATE/gtkgrid.wl"
	# opposite corners of the grid: an offset or a scale error misses one
	GG_BLOCKS="red=255,0,0 cyan=0,255,255"
	# one pointer device for the whole section: a popup's grab must survive
	# from the press that opened it to the click on its item
	rm -f "$STATE/vp.fifo"; mkfifo "$STATE/vp.fifo"
	vp <"$STATE/vp.fifo" & GG_VP=$!
	exec 7>"$STATE/vp.fifo"
	gg_send() { printf '%s\n' "$@" >&7; }
	# gg_click <x> <y> <tag>: click, then wait for the client's next <tag> line
	gg_lines() { grep -c "^$1" "$GG_LOG"; }
	gg_more() { (($(gg_lines "$1") > $2)); }
	gg_click() {
		local n; n="$(gg_lines "$3")"
		gg_send "move $1 $2" "sleep 120" "press 272" "sleep 50" "release 272" "sleep 120"
		until_ok 3 gg_more "$3" "$n"
	}
	# by title: X11 names the class after the script (Gtkgrid.py)
	gg_addr() { clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['title']=='gtkgrid'), ''))"; }
	gg_open() { # gg_open <backend> [trace]
		: >"$GG_LOG"
		dsp "hl.dsp.exec_cmd('python3 $REPO/devtools/gtkgrid.py $GG_LOG $1 ${2:+$GG_WL}')"
		until_ok 10 grep -q READY "$GG_LOG"
		until_ok 10 gg_mapped
	}
	gg_mapped() { [[ -n "$(gg_addr)" ]]; }
	gg_close() { # and wait it out: a leftover window sits on the spots the next checks remember
		local a; a="$(gg_addr)"
		[[ -n "$a" ]] && dsp "hl.dsp.window.close({window=\"address:$a\"})"
		for _ in $(seq 1 30); do [[ -z "$(gg_addr)" ]] && break; sleep 0.1; done
		[[ -z "$(gg_addr)" ]] || bad "the GTK3 client closed"
	}
	gg_find() { python3 "$REPO/devtools/findcolors.py" "$@"; }
	# gg_shot <label> <min-px> <name=r,g,b>...: capture until every color is
	# drawn with at least min-px pixels (a frame or two after a map, a move
	# or a popup), print the finds
	gg_shot() {
		local png="$STATE/gg-$1.png" min=$2 out t
		shift 2
		for t in 1 2 3 4 5 6 7 8; do
			capture_nested "$png"
			out="$(gg_find "$png" "$@")"
			[[ -n "$out" ]] && awk -v m="$min" 'NF < 4 || $4 < m { short = 1 } END { exit short }' <<<"$out" && break
			sleep 0.15
		done
		printf '%s\n' "$out"
	}
	gg_blocks() { # gg_blocks <label>
		local name cx cy n got found=0
		while read -r name cx cy n; do
			found=$((found + 1))
			if ((n < 500)); then bad "[$1] the $name block is drawn ($n px)"; continue; fi
			gg_click "$cx" "$cy" HIT
			got="$(grep '^HIT' "$GG_LOG" | tail -1)"
			[[ "$got" == "HIT $name" ]] && ok "[$1] a click on the drawn $name block hits it" ||
				bad "[$1] a click on the drawn $name block hits it (client: '$got')"
		done < <(gg_shot "$1" 500 $GG_BLOCKS)
		# a failed capture or a crashed finder yields no blocks at all: that
		# ran zero checks, which must not read as passes
		((found > 0)) || bad "[$1] the color blocks were searched for in the capture"
	}
	gg_menu() { # gg_menu <label>: open the menu by its drawn button, click the drawn violet item
		local bx by bn vx vy vn got
		read -r _ bx by bn < <(gg_shot "$1-btn" 500 button=128,64,0)
		if ((bn < 500)); then bad "[$1] the menu button is drawn"; return 1; fi
		gg_click "$bx" "$by" "MENUBUTTON True"
		read -r _ vx vy vn < <(gg_shot "$1-menu" 300 violet=128,0,255)
		if ((vn < 300)); then bad "[$1] the menu opened (violet drawn: $vn px)"; return 1; fi
		gg_click "$vx" "$vy" "MENU "
		got="$(grep '^MENU ' "$GG_LOG" | tail -1)"
		[[ "$got" == "MENU violet" ]] && ok "[$1] a click on the drawn menu item activates it" ||
			bad "[$1] a click on the drawn menu item activates it (client: '$got')"
	}
	gg_cfg() { # the last popup configure: "<y> <h>" relative to the window geometry
		grep -oE 'xdg_popup#[0-9]+\.configure\(-?[0-9]+, -?[0-9]+, [0-9]+, [0-9]+\)' "$GG_WL" | tail -1 |
			sed -E 's/.*\((-?[0-9]+), (-?[0-9]+), ([0-9]+), ([0-9]+)\)/\2 \4/'
	}

	gg_open wayland trace
	gg_blocks gtk3-wl
	if gg_menu gtk3-wl; then
		# near the bottom edge: the menu fits below its button with 6px to
		# spare past the solver's 4px padding, so it must not flip
		read -r CY MH <<<"$(gg_cfg)"
		TY=$((MON_H - 10 - CY - MH))
		dsp "hl.dsp.window.move({x = 200, y = $TY, window = \"address:$(gg_addr)\"})"; settled gtkgrid
		read -r _ GY _ _ <<<"$(box4 gtkgrid)"
		if [[ "$GY" == "$TY" ]]; then
			gg_menu gtk3-wl-edge
			read -r CY2 _ <<<"$(gg_cfg)"
			[[ "$CY2" == "$CY" ]] && ok "a menu that fits below its button at the bottom edge opens below it" ||
				bad "a menu that fits below its button at the bottom edge opens below it (configure y $CY2, unconstrained $CY)"
			# and 12px lower it overflows by 6: it must flip (or slide) up —
			# with the fit above, any solver error past 6px in either
			# direction fails one of the two
			dsp "hl.dsp.window.move({x = 200, y = $((TY + 12)), window = \"address:$(gg_addr)\"})"; settled gtkgrid
			gg_menu gtk3-wl-over
			read -r CY3 _ <<<"$(gg_cfg)"
			((CY3 < CY)) && ok "a menu that overflows the bottom edge by 6px flips above its button" ||
				bad "a menu that overflows the bottom edge by 6px flips above its button (configure y $CY3, unconstrained $CY)"
		else
			bad "the GTK3 window moved near the bottom edge (at y $GY, wanted $TY)"
		fi
	fi
	gg_close
	gg_open x11
	gg_blocks gtk3-x11
	gg_menu gtk3-x11
	gg_close
	exec 7>&-
	wait "$GG_VP" 2>/dev/null
else
	skip "real GTK3 client checks: no GTK3 / PyGObject on this machine"
fi

# A real CSD app's close: the client destroys its toplevel BEFORE the
# window unmaps (splashwin does, like GTK and Firefox). The close must still
# remember the box — the close-time read once needed the live toplevel and
# recorded nothing for every such app (2026-10-04: thunar, firefox and the
# rest reopened centered). The moved spot discriminates: centered is the
# no-memory default.
csdmem_addr() { clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='csdmem'), ''))"; }
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 600 400 20 csdmem - - resz - - - - follow')"; settled csdmem
CM="$(csdmem_addr)"
dsp "hl.dsp.window.move({x = 120, y = 150, window = \"address:$CM\"})"; settled csdmem
close_wait "$CM"
chk "toplevel-first close remembers the spot" until_ok 3 grep -q "^spot	120	150	csdmem$" "$AW_STATE"
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 600 400 20 csdmem - - resz - - - - follow unmaxwhenmaxed')"; settled csdmem
expect "toplevel-first close: the respawn lands at the remembered box" \
	"any(c['class']=='csdmem' and c['at']==[120,150] and c['size']==[600,400] for c in cs)"
# The client's own unmaximize (its CSD titlebar restore button) on a
# plugin-maximized window. The compositor drops it (it only honors an
# unmaximize for a window IT holds maximized); without the plugin's answer
# the client stays told maximized, saves "maximized" and reopens so
# (2026-10-04: firefox "always opens maximized"). The fixture asks 1.5 s
# after it is told maximized.
dsp "hl.plugin.awesome.maximize()"
expect_by 1 "client unmaximize: Mod+M maximized the window first" \
	"any(c['class']=='csdmem' and c['at']==[0,30] and c['size']==[$MON_W,$((MON_H - 30))] for c in cs)"
expect_by 5 "client unmaximize: its own restore button restores the windowed box" \
	"any(c['class']=='csdmem' and c['at']==[120,150] and c['size']==[600,400] for c in cs)"
CM="$(csdmem_addr)"
close_wait "$CM"
chk "csd battery left no windows" test "$(pyc "sum(1 for c in cs if c['class'] in ('csdpin','csdresz','csdpinx','csdfollow','csdfall','csdmem'))")" = 0

# ---- state churn --------------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; settled foot
box() { clients | python3 -c "
import json,sys
f=[ (c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot' ]
print(f[0] if f else 'none')"; }
REF="$(box)"
chk "churn probe up" test "$REF" != none
for _ in $(seq 1 10); do
	hq seterror disable >/dev/null 2>&1
	sleep 0.1
done
dsp "hl.plugin.awesome.maximize()"; settled foot
MAX_BEFORE="$(box)"
RESERVED_BEFORE="$(reserved)"
ERROR_MESSAGE="reserved-area-probe $(printf 'wrapped-message-with-enough-width-to-force-a-second-line %.0s' {1..6})"
hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
for frame_try in $(seq 1 200); do
	RESERVED_WITH_ERROR="$(reserved)"
	[[ "$RESERVED_WITH_ERROR" != "$RESERVED_BEFORE" ]] && break
	hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
	(( frame_try % 10 == 0 )) && capture_nested "$STATE/error-overlay-frame.png" || true
	sleep 0.1
done
if [[ "$RESERVED_WITH_ERROR" != "$RESERVED_BEFORE" ]]; then
	ok "fork: error overlay changes native reserved area"
else
	bad "fork: error overlay changes native reserved area"
fi
for _ in $(seq 1 30); do
	MAX_WITH_ERROR="$(box)"
	[[ "$MAX_WITH_ERROR" != "$MAX_BEFORE" ]] && break
	sleep 0.1
done
if [[ "$MAX_WITH_ERROR" != "$MAX_BEFORE" ]]; then
	ok "maximize: native reserved-area change reflows maximized geometry"
else
	bad "maximize: native reserved-area change reflows maximized geometry"
fi
hq seterror disable >/dev/null 2>&1
RESERVED_AFTER_ERROR=""
for frame_try in $(seq 1 200); do
	RESERVED_AFTER_ERROR="$(reserved)"
	[[ "$RESERVED_AFTER_ERROR" == "$RESERVED_BEFORE" ]] && break
	hq seterror disable >/dev/null 2>&1
	(( frame_try % 10 == 0 )) && capture_nested "$STATE/error-overlay-frame.png" || true
	sleep 0.1
done
if [[ "$RESERVED_AFTER_ERROR" == "$RESERVED_BEFORE" ]]; then
	ok "fork: disabling error overlay restores native reserved area"
else
	bad "fork: disabling error overlay restores native reserved area"
	printf '      expected reserved=%s, got=%s\n' "$RESERVED_BEFORE" "$RESERVED_AFTER_ERROR"
fi
for _ in $(seq 1 30); do
	[[ "$(box)" == "$MAX_BEFORE" ]] && break
	sleep 0.1
done
chk "maximize: removing native reservation restores maximized workarea" test "$(box)" = "$MAX_BEFORE"
dsp "hl.plugin.awesome.maximize()"; settled foot
chk "maximize: reserved-area roundtrip preserves windowed restore" test "$(box)" = "$REF"
for i in $(seq 1 10); do dsp "hl.plugin.awesome.maximize()"; done; settled foot
chk "10 maximize toggles round-trip losslessly" test "$(box)" = "$REF"
for i in $(seq 1 5); do dsp "hl.plugin.awesome.minimize()"; dsp "hl.plugin.awesome.restore()"; done; settled foot
chk "5 minimize/restore cycles round-trip" test "$(box)" = "$REF"

# Mod+U's jump (urgent_or_last: here the last window, the foot on 1)
# switches the view to the window's workspace through the exact-window
# focus AND announces it (the workspace history, IPC bars): `workspace
# previous` returns to where the view was. That focus once switched
# silently and `previous` went nowhere. (A plain focus-window dispatch
# switches the workspace itself first: it never took that path.)
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
dsp "hl.dsp.focus({workspace=\"2\"})"
dsp "hl.dsp.focus({urgent_or_last=true})"
chk "Mod+U jumps to the last window's workspace" test "$(ws)" = 1
dsp "hl.dsp.focus({workspace=\"previous\"})"
chk "that jump was announced: workspace previous returns to 2" test "$(ws)" = 2
dsp "hl.dsp.focus({workspace=\"1\"})"

close_wait "$FF"

# ---- minimize: the successor is the exact window -----------------------
# awesome's autofocus focuses the most recently focused task. The input
# path at the successor's center focused what lay on top there: a small
# window over it (hover focus doesn't raise) took the focus instead.
# By app id: the shell in a foot rewrites its title at the first prompt.
class_addr() {
	clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class'] == sys.argv[1]), ''))" "$1"
}
active_class() { hq activewindow -j | python3 -c 'import json,sys;print(json.load(sys.stdin).get("class",""))'; }
w1_count() { clients | python3 -c "import json,sys;print(sum(1 for c in json.load(sys.stdin) if c['class'] in ('w1base','w1over','w1gone','w1prev')))"; }
hover() { printf 'move %s %s\nsleep 60\n' "$1" "$2" | vp; settle; }
dsp "hl.dsp.exec_cmd('foot --app-id=w1base --window-size-pixels=600x400')"
dsp "hl.dsp.exec_cmd('foot --app-id=w1over --window-size-pixels=200x120')"
dsp "hl.dsp.exec_cmd('foot --app-id=w1gone --window-size-pixels=240x160')"
dsp "hl.dsp.exec_cmd('foot --app-id=w1prev --window-size-pixels=240x160')"
until_is 10 4 w1_count
W1BASE="$(class_addr w1base)" W1OVER="$(class_addr w1over)" W1GONE="$(class_addr w1gone)" W1PREV="$(class_addr w1prev)"
dsp "hl.dsp.window.move({x = 100, y = 100, window = \"address:$W1BASE\"})"
dsp "hl.dsp.window.move({x = 300, y = 240, window = \"address:$W1OVER\"})"
dsp "hl.dsp.window.move({x = 800, y = 100, window = \"address:$W1GONE\"})"
dsp "hl.dsp.window.move({x = 800, y = 400, window = \"address:$W1PREV\"})"
hover 400 300 # the base's center: the small window is on top there
chk "minimize successor: the small window covers the base's center" test "$(active_class)" = w1over
hover 900 480
hover 150 150
hover 900 150
chk "minimize successor: focus went over, prev, base, then the one to go" test "$(active_class)" = w1gone
dsp "hl.plugin.awesome.minimize()"
chk "minimize successor: the most recent window takes the focus, not the one over its center" until_is 2 w1base active_class
expect "minimize successor: it is focused unraised (the small window stays over it)" \
	"[c['class'] for c in cs].index('w1over') > [c['class'] for c in cs].index('w1base')"
# the first minimize never focused the window over the base's center:
# the history still has prev before base, not over
dsp "hl.plugin.awesome.minimize()"
chk "minimize successor: a second minimize goes back past the covering window" until_is 2 w1prev active_class
for a in "$W1BASE" "$W1OVER" "$W1GONE" "$W1PREV"; do
	[[ -n "$a" ]] && dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
until_is 5 0 w1_count
chk "minimize successor: its windows are closed" test "$(w1_count)" = 0
# ---- maximize: the entry follows its window out of floating -------------
# A swap with a tiled window trades the two floating states without the
# layout manager's toggle event: the maximized window turns tiled, and its
# entry has to leave with it. A stale one turns the next Mod+M on the
# refloated window into a restore of the old box instead of a maximize.
# (w2tile is not in the harness float rule, w2max is.)
w2_count() { clients | python3 -c "import json,sys;print(sum(1 for c in json.load(sys.stdin) if c['class'] in ('w2tile','w2max')))"; }
w2_state() { # w2_state <class> <floating|tiled|wide>: 1 when the window is so
	clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class'] == sys.argv[1]), None)
w = {'floating': c and c['floating'], 'tiled': c and not c['floating'], 'wide': c and c['size'][0] > $((MON_W-8))}[sys.argv[2]]
print(1 if w else 0)" "$1" "$2"
}
dsp "hl.dsp.exec_cmd('foot --app-id=w2tile --window-size-pixels=500x300')"
dsp "hl.dsp.exec_cmd('foot --app-id=w2max --window-size-pixels=500x300')"
until_is 10 2 w2_count
W2TILE="$(class_addr w2tile)" W2MAX="$(class_addr w2max)"
chk "maximize exit: w2tile tiles, w2max floats" \
	test "$(w2_state w2tile tiled)$(w2_state w2max floating)" = 11
dsp "hl.dsp.focus({window=\"address:$W2MAX\"})"
dsp "hl.plugin.awesome.maximize()"
until_is 5 1 w2_state w2max wide
chk "maximize exit: w2max fills the workarea" test "$(w2_state w2max wide)" = 1
dsp "hl.dsp.window.swap({window=\"address:$W2MAX\", target=\"address:$W2TILE\"})"
until_is 5 1 w2_state w2max tiled
chk "maximize exit: the swap tiled w2max" test "$(w2_state w2max tiled)" = 1
dsp "hl.dsp.window.float({action=\"enable\", window=\"address:$W2MAX\"})"
until_is 5 1 w2_state w2max floating; settled w2max
dsp "hl.dsp.focus({window=\"address:$W2MAX\"})"
dsp "hl.plugin.awesome.maximize()"
until_is 5 1 w2_state w2max wide
chk "maximize exit: floated again, Mod+M maximizes it (no stale entry)" \
	test "$(w2_state w2max floating)$(w2_state w2max wide)" = 11
for a in "$W2TILE" "$W2MAX"; do
	[[ -n "$a" ]] && dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
until_is 5 0 w2_count

# ---- a switch to a minimized window restores it -------------------------
# Mod+U (urgent_or_last) or a focus dispatch naming a minimized window is
# the user's pick: restored and focused, as awesome's jump_to un-minimizes.
# The compositor's focus once landed on the hidden window and the plugin
# bounced it: the window stayed minimized. Two restores in one dispatch
# restore two windows (one re-armed hop once dropped the first).
hidden_of() { clients | python3 -c "
import json,sys
print(next((int(c['hidden']) for c in json.load(sys.stdin) if c['class'] == sys.argv[1]), -1))" "$1"; }
w3_count() { clients | python3 -c "import json,sys;print(sum(1 for c in json.load(sys.stdin) if c['class'] in ('w3a','w3b','w3c')))"; }
w3b_state() { echo "$(hidden_of w3b) $(active_class)"; } # hidden, and who has the focus
w3_hidden() { echo "$(hidden_of w3b) $(hidden_of w3c)"; }
# one at a time: the focus history's order is the opening order (Mod+U's
# "last" is the minimized w3b below whichever end the history counts from)
for c in w3a w3b w3c; do
	dsp "hl.dsp.exec_cmd('foot --app-id=$c --window-size-pixels=300x200')"
	wait_class "$c"
	until_is 2 "$c" active_class
done
W3A="$(class_addr w3a)" W3B="$(class_addr w3b)" W3C="$(class_addr w3c)"
dsp "hl.dsp.focus({window=\"address:$W3B\"})"
until_is 2 w3b active_class
dsp "hl.plugin.awesome.minimize()"
chk "jump: the minimized window hides and hands the focus on" until_is 2 "1 w3c" w3b_state
dsp "hl.dsp.focus({urgent_or_last=true})"
chk "jump: Mod+U to the minimized last window restores and focuses it" until_is 2 "0 w3b" w3b_state
dsp "hl.plugin.awesome.minimize()"
until_is 2 "1 w3c" w3b_state
dsp "hl.dsp.focus({window=\"address:$W3B\"})"
chk "jump: a focus dispatch naming a minimized window restores it" until_is 2 "0 w3b" w3b_state
dsp "hl.plugin.awesome.minimize()"
until_is 2 w3c active_class
dsp "hl.plugin.awesome.minimize()"
until_is 2 "1 1" w3_hidden
hq eval "hl.plugin.awesome.restore(); hl.plugin.awesome.restore()" >/dev/null 2>&1
chk "restore: two in one dispatch restore both" until_is 2 "0 0" w3_hidden
for a in "$W3A" "$W3B" "$W3C"; do
	[[ -n "$a" ]] && dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
until_is 5 0 w3_count

chk "windows: its foot is closed at the battery's end" \
	test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0

