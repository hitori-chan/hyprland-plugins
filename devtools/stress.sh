#!/usr/bin/env bash
# devtools/stress.sh — the pre-deploy regression gate. Boots the full plugin
# stack in the nested harness and drives it through the storm battery:
# placement memory, sibling geometry, spawn/close storms, the notification
# cap, state churn round-trips, hostile state files, a real-input storm
# (vptr) and the click-corpse guard. Every assertion is exact; any failure
# fails the run.
#
#   stress.sh [HYPR_BIN]     default /usr/local/bin/Hyprland — pass a fork
#                            build (e.g. ~/repo/Hyprland/build/Hyprland) to
#                            gate an uninstalled compositor; PKG_CONFIG_PATH
#                            is honored so plugins can build against scratch
#                            headers of that same tree.
#
# Needs the nested harness at ~/.local/share/hypr-nested (launch.sh) and a
# live Wayland session to host the nested window. Leaves the live session
# untouched.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="${HYPR_HARNESS:-$HOME/.local/share/hypr-nested}"
BIN="${1:-${HYPR_BIN:-/usr/local/bin/Hyprland}}"
STATE="$HARNESS/stress-state"
CFG="$HARNESS/stress.lua"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""

PASS=0
FAILED=()
ok()  { PASS=$((PASS + 1)); printf '  ok  %s\n' "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL %s\n' "$1"; }
chk() { # chk <name> <command...> — command's exit code decides
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
}

hq()      { hyprctl -i "$SIG" "$@"; }
dsp()     { hq dispatch "$1" >/dev/null 2>&1; }
clients() { hq clients -j 2>/dev/null; }
# pyc <python-expr-over-cs> — cs = client list; truthy stdout "1" = pass
pyc() { clients | python3 -c "
import json,sys
cs=json.load(sys.stdin)
print(1 if ($1) else 0)" ; }
expect() { # expect <name> <python-expr-over-cs>
	[[ "$(pyc "$2")" == "1" ]] && ok "$1" || bad "$1"
}

kill_nested() { # kill any non-live instance running one of the harness cfgs
	for s in "$RUNDIR"/*/; do
		local sig pid
		sig="$(basename "$s")"
		[[ "$sig" == "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] && continue
		pid="$(head -1 "$s/hyprland.lock" 2>/dev/null)"
		[[ -n "$pid" ]] || continue
		grep -qa -- "$HARNESS" "/proc/$pid/cmdline" 2>/dev/null && kill "$pid" 2>/dev/null
	done
	sleep 0.6
}

echo "== stress: $BIN =="

# ---- preflight ----------------------------------------------------------
[[ -x "$BIN" ]] || { echo "no such compositor binary: $BIN"; exit 1; }
[[ -x "$REPO/devtools/vptr" ]] || make -C "$REPO/devtools" >/dev/null
vsync_ok=1
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
	TOML=$(grep -A2 "^\[$p\]" "$REPO/hyprpm.toml" | grep version | grep -o '[0-9.]*')
	SRC=$(grep -rhoE '"[0-9]+\.[0-9]+\.[0-9]+"' "$REPO/$p/main.cpp" "$REPO/$p"/*.hpp 2>/dev/null | tail -1 | tr -d '"')
	[[ "$TOML" == "$SRC" ]] || { vsync_ok=0; echo "  version skew: $p toml=$TOML src=$SRC"; }
done
[[ $vsync_ok == 1 ]] && ok "version sync (toml == PLUGIN_INIT), all 8" || bad "version sync"

# ---- build + launch -----------------------------------------------------
kill_nested
# deploy rehearsal FIRST: hyprpm builds against ITS cached headers (the
# system-default pkg-config resolution), not this run's scratch set — a
# plugin that cannot build there bricks the whole hyprpm swap (hyprplace
# 2.0.1 did). These throwaway builds are overwritten just below.
dep_ok=1
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
	env -u PKG_CONFIG_PATH make -B -C "$REPO/$p" >/dev/null 2>&1 || { dep_ok=0; echo "  deploy-build broke: $p"; }
done
[[ $dep_ok == 1 ]] && ok "deploy rehearsal: all 8 build against the installed header cache" || bad "deploy rehearsal build"
# now the real builds for this run's compositor (caller's PKG_CONFIG_PATH)
build_ok=1
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
	make -B -C "$REPO/$p" >/dev/null 2>&1 || { build_ok=0; echo "  build broke: $p"; }
done
[[ $build_ok == 1 ]] && ok "all 8 plugins build" || { echo "plugin build FAILED"; exit 1; }
rm -rf "$STATE"; mkdir -p "$STATE/hyprplace"
printf '100\t100\t500\t400\tfoot\n200\t80\tlegacyfoot\n' > "$STATE/hyprplace/lastspot.tsv"
{ cat "$HARNESS/nested.lua"; echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB" }, float = true })'; } > "$CFG"
HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" bash "$HARNESS/launch.sh" >/dev/null 2>&1 || { echo "nested launch FAILED"; exit 1; }
SIG="$(cat "$HARNESS/nested.sig")"
LOG="$HARNESS/nested.log"
chk "8 plugins loaded" test "$(hq plugin list | grep -c Plugin)" = 8
dsp "hl.dsp.window.close()" # the donate/updated screen, when present
sleep 0.5

# ---- placement memory ---------------------------------------------------
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "size memory: remembered 500x400 beats requested 600x300 at (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "sibling is born at the remembered 500x400 too" \
	"sum(1 for c in cs if c['class']=='foot' and c['size']==[500,400])==2"
expect "sibling lands off the taken spot — no exact stacking" \
	"len(set(tuple(c['at']) for c in cs if c['class']=='foot'))==2"
# the spot is occupied, not retired: free it and the next sibling takes it
A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at']==[100,100]), ''))")"
dsp "hl.dsp.window.close({window=\"address:$A\"})"; sleep 1
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "freed spot reclaimed: next sibling lands at (100,100) 500x400" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"

# fullscreen roundtrip on the focused (newest) foot
feet() { clients | python3 -c "
import json,sys
print(sorted((c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot'))"; }
FEET="$(feet)"
dsp "hl.dsp.window.fullscreen()"; sleep 0.7; dsp "hl.dsp.window.fullscreen()"; sleep 0.9
chk "fullscreen roundtrip restores the exact boxes" test "$(feet)" = "$FEET"

# ---- close storm + memory update ---------------------------------------
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 2
chk "close storm: no stragglers" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0
chk "tsv: exactly one foot row survives the coalesced save" test "$(grep -c $'\tfoot$' "$STATE/hyprplace/lastspot.tsv")" = 1
chk "tsv: no temp-file debris" bash -c "! ls $STATE/hyprplace/*.tmp 2>/dev/null | grep -q ."

# ---- spawn storm --------------------------------------------------------
for i in $(seq 1 12); do
	dsp "hl.dsp.exec_cmd('foot --window-size-pixels=$((400 + (i % 4) * 80))x$((250 + (i % 3) * 60))')" &
done; wait; sleep 4
expect "spawn storm: all 12 up, fully inside the workarea" \
	"sum(1 for c in cs if c['class']=='foot')==12 and all(c['at'][0]>=0 and c['at'][1]>=26 and c['at'][0]+c['size'][0]<=1280 and c['at'][1]+c['size'][1]<=800 for c in cs if c['class']=='foot')"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 2

# ---- notification cap ---------------------------------------------------
for i in $(seq 1 65); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" &
done; wait; sleep 5
chk "notif storm: cap holds at exactly 50/65" test "$(hq hyprnotify count)" = 50
chk "notif storm: the 15 evicted land in history" test "$(hq hyprnotify history)" = 15
for r in 1 2 3 4 5; do hq hyprnotify recall >/dev/null; done; sleep 2
chk "recall churn is net-zero (count stays 50)" test "$(hq hyprnotify count)" = 50
# wrong-typed hints make sdbus-c++ throw inside the plugin's parse — the
# catch must survive (exercises exception unwinding across the .so boundary).
# Cards expire on their own clocks, so assert the daemon still answers with
# a number, not any absolute count.
dsp "hl.dsp.exec_cmd('notify-send -h int:transient:1 -h string:urgency:critical typed-hint-abuse body')"; sleep 1.5
chk "wrong-typed hints survived (sdbus::Error thrown + caught)" bash -c "hyprctl -i $SIG hyprnotify count | grep -qE '^[0-9]+$'"

# ---- state churn --------------------------------------------------------
# the spawn box is whatever memory dictates after the storm above — capture
# it, then assert every churn round-trips back to exactly that box
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
box() { clients | python3 -c "
import json,sys
f=[ (c['at'],c['size']) for c in json.load(sys.stdin) if c['class']=='foot' ]
print(f[0] if f else 'none')"; }
REF="$(box)"
chk "churn probe up" test "$REF" != none
for i in $(seq 1 20); do dsp "hl.plugin.hyprmax.toggle()"; done; sleep 1
chk "20 maximize toggles round-trip losslessly" test "$(box)" = "$REF"
for i in $(seq 1 10); do dsp "hl.plugin.hyprbar.minimize()"; dsp "hl.plugin.hyprbar.restore()"; done; sleep 1
chk "10 minimize/restore cycles round-trip" test "$(box)" = "$REF"
for i in $(seq 1 30); do dsp "hl.dsp.focus({workspace=\"$(( (i % 9) + 1 ))\"})"; done
dsp "hl.dsp.focus({workspace=\"1\"})"; sleep 1
chk "30 workspace hops: back on 1" bash -c "hyprctl -i $SIG activeworkspace -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)[\"id\"]==1 else 1)'"

# ---- hostile state file -------------------------------------------------
kill_nested
printf 'garbage\n42\n1e400\t0\t300\t200\tinffoot\n-100\t-100\t-50\t-50\tnegfoot\n100000\t100000\t400\t300\tfoot\n' > "$STATE/hyprplace/lastspot.tsv"
HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" bash "$HARNESS/launch.sh" >/dev/null 2>&1 || { echo "relaunch FAILED"; exit 1; }
SIG="$(cat "$HARNESS/nested.sig")"
chk "hostile tsv: all 8 plugins still load" test "$(hq plugin list | grep -c Plugin)" = 8
dsp "hl.dsp.window.close()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
# the stored 400x300 is applied over the requested 500x300, then the
# 100000,100000 spot clamps to the margin corner for THAT size
expect "far-off-screen seed: stored size applied, clamped to (879,499)" \
	"any(c['class']=='foot' and c['at']==[879,499] and c['size']==[400,300] for c in cs)"

# ---- real-input storm ---------------------------------------------------
WL="$(cat "$HARNESS/nested.wl")"
{
	for i in $(seq 1 60); do echo "move $(( (i * 97) % 1280 )) $(( 30 + (i * 61) % 760 ))"; echo "sleep 10"; done
	for i in $(seq 1 15); do echo "move 500 13"; echo "sleep 15"; echo "scroll 0 1"; echo "sleep 25"; done
	for i in $(seq 1 10); do
		echo "move 34 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
		echo "move 59 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
	done
	echo "move 12 13"; echo "sleep 30"; echo "press 272"; echo "sleep 30"; echo "release 272"; echo "sleep 100"
} | WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" >/dev/null 2>&1
sleep 1
chk "input storm: all 8 plugins alive" test "$(hq plugin list | grep -c Plugin)" = 8
chk "input storm: the final taglist click registered (ws 1)" bash -c "hyprctl -i $SIG activeworkspace -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)[\"id\"]==1 else 1)'"
printf 'move 250 200\nsleep 50\npress 272\nsleep 40\nrelease 272\nsleep 100\n' | WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" >/dev/null 2>&1
sleep 0.8
expect "post-storm click still raises + focuses (no stuck swallow)" \
	"cs[-1]['class']=='foot' if cs else False"

# ---- corpse guard (hyprclick) -------------------------------------------
# the tail of a fast double-click on a click-to-close window (Telegram's
# image viewer backdrop) lands after the unmap: it must be swallowed, not
# focus-and-raise whatever sat beneath (it flipped the live stack).
dsp "hl.dsp.exec_cmd('foot -a corpseA')"; sleep 1.6
dsp "hl.dsp.exec_cmd('foot -a corpseB')"; sleep 1.6
dsp "hl.dsp.exec_cmd('foot -a corpseB -F')"; sleep 1.6
# a point over corpseA that corpseB doesn't cover: the press that must die
P="$(clients | python3 -c "
import json,sys
cs=json.load(sys.stdin)
A=next(c for c in cs if c['class']=='corpseA')
B=next(c for c in cs if c['class']=='corpseB' and c['fullscreen']==0)
def inside(p,c): return c['at'][0]<=p[0]<c['at'][0]+c['size'][0] and c['at'][1]<=p[1]<c['at'][1]+c['size'][1]
pts=[(A['at'][0]+dx,A['at'][1]+dy) for dx in (30,A['size'][0]-30) for dy in (30,A['size'][1]-30)]
x,y=next(p for p in pts if not inside(p,B))
print(x,y)")"
V="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['fullscreen']==2))")"
# a 5-press burst spanning ~700ms: the tail outlives a fixed 400ms corpse,
# so this also asserts each swallowed press extends the gesture
( { printf "move ${P% *} ${P#* }\nsleep 30\n"; for i in 1 2 3 4 5; do printf "press 272\nsleep 40\nrelease 272\nsleep 110\n"; done; } | WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" >/dev/null 2>&1 ) &
sleep 0.12; dsp "hl.dsp.window.close({window=\"address:$V\"})"
wait; sleep 0.9
expect "corpse guard: click burst through a dying viewer keeps the stack" \
	"cs[-1]['class']=='corpseB'"
chk "corpse guard: focus stayed with the viewer's app" bash -c \
	"hyprctl -i $SIG activewindow -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)[\"class\"]==\"corpseB\" else 1)'"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class'] in ('corpseA','corpseB')]"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})"
done
sleep 0.8

# ---- log hygiene --------------------------------------------------------
chk "log clean (only known-benign lines)" bash -c \
	"! grep -iE 'error|assert|segv|abort' '$LOG' | grep -vE 'Creating the Error Overlay|xkbcomp' | grep -q ."

# ---- teardown -----------------------------------------------------------
kill_nested
rm -rf "$STATE" "$CFG"
hyprctl output remove nested-dev >/dev/null 2>&1
rm -f "$HARNESS/nested.sig" "$HARNESS/nested.wl"

echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
	echo "== stress: ALL $PASS CHECKS PASSED =="
	exit 0
else
	echo "== stress: $PASS passed, ${#FAILED[@]} FAILED =="
	printf '   - %s\n' "${FAILED[@]}"
	exit 1
fi
