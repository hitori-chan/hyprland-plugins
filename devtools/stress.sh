#!/usr/bin/env bash
# devtools/stress.sh — the pre-deploy regression gate. Boots the full plugin
# stack in the nested harness and drives it through the storm battery:
# placement memory, sibling geometry, spawn/close storms, the notification
# cap, state churn round-trips, hostile state files, a real-input storm
# (vptr), the shade's click and key verbs (vkbd), the bell's hover-peek, the
# click-corpse guard and the fullscreen tuck. Every assertion is exact; any
# failure fails the run.
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

# Nothing here may hard-code the nested monitor's size: it is whatever window
# the wayland backend gets, and it HAS changed under us (the 1280x800 these
# coordinates assumed is now 1920x1200). retarget re-reads the instance after
# every launch and vp injects through vptr with that real extent — vptr maps
# `move X Y` as X/extent onto the output, so a wrong extent silently lands
# every scripted click somewhere else and the assertion passes or fails on
# whatever happened to be under it.
WL=""; MON_W=0; MON_H=0; NBUS=""
retarget() {
	SIG="$(cat "$HARNESS/nested.sig")"
	WL="$(cat "$HARNESS/nested.wl")"
	# launch.sh isolates the nested instance under its OWN dbus-run-session,
	# so anything driving the nested daemon over the bus must use THAT
	# address: the login session's bus is owned by the host's hyprnotify,
	# which answers happily and makes the assertion vacuous.
	local pid
	pid="$(head -1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
	NBUS="$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')"
	read -r MON_W MON_H < <(hq monitors -j | python3 -c "
import json,sys
m=json.load(sys.stdin)[0]
print(int(m['width']/m['scale']), int(m['height']/m['scale']))")
}
vp() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1; }
vk() { WAYLAND_DISPLAY="$WL" "$REPO/devtools/vkbd" >/dev/null 2>&1; } # keys need no extent
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
{ [[ -x "$REPO/devtools/vptr" ]] && [[ -x "$REPO/devtools/vkbd" ]]; } || make -C "$REPO/devtools" >/dev/null
# the headers pkg-config resolves must belong to the gated binary — a
# scratch hyprland.pc keeps its absolute /usr/local prefix (not
# relocatable), silently falls back to the installed tree, and every
# plugin embeds the wrong hash: all 8 mismatch-throw at load. Rewrite the
# scratch pc's prefix= to its own include/ before gating a fork build.
HDR_ROOT="$(pkg-config --cflags hyprland 2>/dev/null | tr ' ' '\n' | grep '^-I' | head -1 | sed 's/^-I//')"
HDR_HASH="$(grep -h GIT_COMMIT_HASH "$HDR_ROOT/hyprland/src/version.h" 2>/dev/null | grep -oE '[0-9a-f]{40}')"
BIN_HASH="$("$BIN" --version 2>/dev/null | grep -oE 'commit [0-9a-f]{40}' | cut -d' ' -f2)"
if [[ -n "$HDR_HASH" && "$HDR_HASH" == "$BIN_HASH" ]]; then
	ok "headers match the gated binary (${BIN_HASH:0:8})"
else
	bad "headers match the gated binary (headers ${HDR_HASH:0:8} vs binary ${BIN_HASH:0:8})"
	echo "   header root: $HDR_ROOT"; echo "   refusing to run a gate that mismatches at load"; exit 1
fi
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
{ cat "$HARNESS/nested.lua"; echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs" }, float = true })'; } > "$CFG"
HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" bash "$HARNESS/launch.sh" >/dev/null 2>&1 || { echo "nested launch FAILED"; exit 1; }
retarget
LOG="$HARNESS/nested.log"
ok "nested monitor is ${MON_W}x${MON_H} (every coordinate below derives from it)"
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
	"sum(1 for c in cs if c['class']=='foot')==12 and all(c['at'][0]>=0 and c['at'][1]>=26 and c['at'][0]+c['size'][0]<=$MON_W and c['at'][1]+c['size'][1]<=$MON_H for c in cs if c['class']=='foot')"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class']=='foot']"); do
	dsp "hl.dsp.window.close({window=\"address:$a\"})" &
done; wait; sleep 2

# ---- notification cap ---------------------------------------------------
for i in $(seq 1 65); do
	u=normal; [[ $((i % 6)) == 0 ]] && u=critical
	dsp "hl.dsp.exec_cmd('notify-send -u $u \"stress $i\" body')" &
done; wait; sleep 5
chk "notif storm: cap holds at exactly 50/65" test "$(hq hyprnotify count)" = 50
# the shade has no history: the 15 evicted are gone, and the verbs that used
# to resurrect them are gone with it (6.0.0)
chk "no history verb survives the model removal" test "$(hq hyprnotify history)" = "unknown request"
chk "no recall verb survives the model removal" test "$(hq hyprnotify recall)" = "unknown request"
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
# the policy store is the other user-editable file: a verb-less line, an
# empty key and an unknown verb must all be skipped, not fatal
mkdir -p "$STATE/hyprnotify"
printf 'garbage\ns\n s\tx\nz\tnope\ns\t\ns\tkeepme\n' > "$STATE/hyprnotify/policy.tsv"
HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" bash "$HARNESS/launch.sh" >/dev/null 2>&1 || { echo "relaunch FAILED"; exit 1; }
retarget
chk "hostile tsv: all 8 plugins still load" test "$(hq plugin list | grep -c Plugin)" = 8
chk "hostile policy: only the well-formed rule loaded" test "$(hq hyprnotify policy)" = "silenced:1 s=keepme priority:0"
dsp "hl.dsp.window.close()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
# the stored 400x300 is applied over the requested 500x300, then the
# 100000,100000 spot clamps to the bottom-right margin corner for THAT size
expect "far-off-screen seed: stored size applied, clamped to ($((MON_W-401)),$((MON_H-301)))" \
	"any(c['class']=='foot' and c['at']==[$((MON_W-401)),$((MON_H-301))] and c['size']==[400,300] for c in cs)"

# ---- expiry, residency & the center ------------------------------------
# The 5.3.0 model: a normal banner runs its clock, then RETREATS to a
# resident shade row — still in the model (the center is the safety net),
# just no longer a popup. Critical (urgency>=2) is the only sticky banner.
# Ephemerals vanish outright: transient and progress/OSD. The `state` line
# counts residents as `live` (raw model size, blind to the popup/shade
# split); the `badge` verb reads that split — "banners:N resident:N".
# Distinct -a apps here so popup coalescing (tested below) can't interfere.
st() { hq hyprnotify state; }
bd() { hq hyprnotify badge; }
chk "notif reset: clean state line" test "$(st)" = "center:0 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a crit -u critical \"urgent\" body')" # no -t: critical sticks
sleep 1
chk "critical: a sticky banner, nothing kept yet" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.exec_cmd('notify-send -a norm -t 600 normal body')" # explicit clock; retreats fast for the test
sleep 1.2
chk "residency: the expired normal banner retreated to a resident row" test "$(bd)" = "banners:1 resident:1"
chk "residency: the retreated card stays in the model, not lost" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a low -u low -t 600 lowcard body')" # low: retreats like a normal card
dsp "hl.dsp.exec_cmd('notify-send -a tran -e -t 600 trans body')"       # transient: VANISHES
sleep 1.2
chk "ephemerals: transient vanished, low parked resident" test "$(st)" = "center:0 live:3 dnd:0"
chk "ephemerals: only the critical banner is still up" test "$(bd)" = "banners:1 resident:2"
hq hyprnotify center >/dev/null; sleep 0.5
chk "center: opening absorbs the banner into the shade" test "$(bd)" = "banners:0 resident:3"
chk "center: opening dismisses nothing" test "$(st)" = "center:1 live:3 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.5
chk "center: closing neither re-pops nor drops a card" test "$(st)" = "center:0 live:3 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
chk "center: Clear all sweeps every visible card" test "$(st)" = "center:0 live:0 dnd:0"
# THE 5.3.0 default: a plain (-1) normal card is no longer sticky — it runs
# timeout_normal (5s) and retreats to the shade on its own, unattended.
dsp "hl.dsp.exec_cmd('notify-send \"default normal\" body')"
sleep 1
chk "default normal: pops as a banner" test "$(bd)" = "banners:1 resident:0"
sleep 5
chk "default normal: retreated to the shade after timeout_normal" test "$(bd)" = "banners:0 resident:1"
chk "default normal: the card is kept, not lost" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- popup coalescing: one live banner per app -------------------------
# spam control (coalesce_popups, default on): while an app shows a banner,
# further NON-CRITICAL same-app arrivals are born resident — one popup, the
# rest silent in the shade's fold. `badge` sees the split, `state` counts
# them all. Critical punches through; a different app gets its own banner.
dsp "hl.dsp.exec_cmd('notify-send -a chatty -t 30000 first body')"; sleep 1
chk "coalesce: the first same-app arrival pops a banner" test "$(bd)" = "banners:1 resident:0"
dsp "hl.dsp.exec_cmd('notify-send -a chatty second body')"
dsp "hl.dsp.exec_cmd('notify-send -a chatty third body')"; sleep 1
chk "coalesce: the same-app burst lands resident, one banner stands" test "$(bd)" = "banners:1 resident:2"
chk "coalesce: every card is kept and counted" test "$(st)" = "center:0 live:3 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a chatty -u critical urgent body')"; sleep 1
chk "coalesce: a critical from the same app punches through" test "$(bd)" = "banners:2 resident:2"
dsp "hl.dsp.exec_cmd('notify-send -a other elsewhere body')"; sleep 1
chk "coalesce: a different app gets its own banner" test "$(bd)" = "banners:3 resident:2"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the conversation merge (Android's MessagingStyle) ------------------
# one chat is ONE card however many messages arrive: a fresh Notify whose
# app + summary matches a live card is joined onto it. The fd.o conversation
# categories are the trigger (the summary is the sender), so a plain card
# from the same app must NOT be swallowed.
for m in one two three; do dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Alice \"$m\"')"; sleep 0.4; done
sleep 0.8
chk "merge: 3 messages from one sender collapse to 1 card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -c im.received -t 30000 Bob hello')"; sleep 1
chk "merge: a different sender keeps its own card" test "$(st)" = "center:0 live:2 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain one\" body')"
dsp "hl.dsp.exec_cmd('notify-send -a tg -t 30000 \"plain two\" body')"; sleep 1
chk "merge: no category, no merging — same app still stacks" test "$(st)" = "center:0 live:4 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# shade overflow: more rows than the monitor-tall panel holds must PAGE, not
# bleed off the bottom. 15 distinct-app cards -> 15 rows (one app each, so
# nothing bundles); the expansion budget opens what fits and folds the rest.
# Drawing it (the placement break + the "▾ N" paging cue) must not crash and
# must keep every card.
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq hyprnotify center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- hardening: the shade's click model, absorb, DND, hostile hints -----
# A shade row IS its banner: left on the BODY fires the card's primary and
# dismisses it, and the CHEVRON is the only fold target. Driven through the
# real hit boxes via vptr. The panel hangs off the monitor's right edge
# (EDGE 10 + CENTER_W 360) below offset_y 34, so the first row's body is a
# fixed inset from the top-right corner, and the chevron rides the row's
# right end (ROW_PADX 12 + CHEV 24). Hit boxes are final-position, so the
# open spring cannot move them out from under the click. The fold has no
# model-level path, hence the shape of these assertions: the chevron must
# change NOTHING in the model, while the body clears the card.
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"read me\" body')"; sleep 1
chk "shade: one card waiting" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.6
click() { # click <x> <y> <button-code>
	printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
		vp
	sleep 0.8
}
ROWX=$((MON_W - 10 - 360 + 10 + 80)) # panel x + body pad + into the text column
ROWY=64                              # offset_y + body pad + into the first row
CHVX=$((MON_W - 10 - 10 - 12 - 12))  # panel right edge - body pad - ROW_PADX - half CHEV
click $CHVX $ROWY 272
chk "shade: the chevron only folds — nothing invoked, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $CHVX $ROWY 272
chk "shade: the chevron unfolds again, still nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "shade: left on the BODY fires the card and it goes, as on the banner" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- hover holds a banner's clock ------------------------------------------
# A card must not expire out from under the pointer reading it. The pointer
# parks on the popup (top-right: EDGE 10 + width 348, below offset_y 34) for
# longer than the card's own timeout, then leaves — which RESTARTS the full
# clock rather than resuming the sliver that was left.
POPX=$((MON_W - 10 - 348 / 2))
dsp "hl.dsp.exec_cmd('notify-send -t 1200 \"hold me\" body')"; sleep 0.4
printf 'move %s 64\nsleep 2400\n' "$POPX" | vp
chk "hover: the pointer holds the banner past its own clock" test "$(bd)" = "banners:1 resident:0"
printf 'move %s %s\nsleep 150\n' "$((MON_W / 2))" "$((MON_H / 2))" | vp
sleep 0.4
chk "hover: leaving restarts the clock, it has not expired yet" test "$(bd)" = "banners:1 resident:0"
sleep 1.4
chk "hover: once the restarted clock runs out it retreats" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- the bell's hover-peek --------------------------------------------------
# Driven over the very bus verb hyprbar's bell calls. A peek opens the shade
# UNPINNED and must NOT absorb: a pointer crossing the bell cannot be allowed
# to swallow banners the user never read. Leaving closes it again after the
# grace; a toggle (the bell's click) pins instead of closing.
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
peek() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify Peek b "$1" >/dev/null 2>&1; }
# guard the whole battery against passing vacuously: if the call lands on the
# WRONG daemon (or none), every "the shade stayed shut" assertion below is
# true for the wrong reason
chk "peek: the nested daemon is the one answering, and it has Peek" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify | grep -q '\.Peek'"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"peek me\" body')"; sleep 1
chk "peek: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
peek true; sleep 0.6
chk "peek: hovering the bell opens the shade" test "$(st)" = "center:1 live:1 dnd:0"
chk "peek: a peek does NOT absorb the banner" test "$(bd)" = "banners:1 resident:0"
peek false; sleep 1.2 # > the 400ms grace
chk "peek: leaving the bell closes it again" test "$(st)" = "center:0 live:1 dnd:0"
peek true; sleep 0.6
hq hyprnotify center >/dev/null; sleep 0.5
chk "peek: the bell's click PINS rather than closing" test "$(st)" = "center:1 live:1 dnd:0"
chk "peek: pinning absorbs what the peek left alone" test "$(bd)" = "banners:0 resident:1"
peek false; sleep 1.2
chk "peek: a pinned shade ignores the pointer leaving" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "peek: reset after the peek battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the shade's keyboard nav ----------------------------------------------
# The one surface with no pointer path at all. Injected through a REAL virtual
# keyboard so it rides the same emission a physical key does. The list is
# newest-first, so ↓ lands on "key two" and the card carrying the primary is
# behind it. The destructive steps are positive assertions on purpose: a dead
# injector would make every "nothing changed" line pass for the wrong reason.
tap() { printf 'tap %s\nsleep 250\n' "$1" | vk; sleep 0.6; }
dsp "hl.dsp.exec_cmd('notify-send -t 60000 -A default=Open \"key one\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"key two\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "keys: two rows with the shade open" test "$(st)" = "center:1 live:2 dnd:0"
tap down
chk "keys: down only SELECTS — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:2 dnd:0"
tap space
chk "keys: space only folds" test "$(st)" = "center:1 live:2 dnd:0"
tap delete
chk "keys: delete dismisses the selected row" test "$(st)" = "center:1 live:1 dnd:0"
tap enter
chk "keys: enter fires the primary and the card goes" test "$(st)" = "center:1 live:0 dnd:0"
tap esc
chk "keys: esc closes the shade" test "$(st)" = "center:0 live:0 dnd:0"

# ---- quiet while fullscreen -------------------------------------------------
# A real fullscreen window owns the screen, so the banner is held back and the
# card lands straight in the shade (residency is the safety net, so nothing is
# lost). Critical still punches through, exactly as it does through DND.
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x400')"; sleep 2
dsp "hl.dsp.window.fullscreen()"; sleep 1
# mode 2 is FSMODE_FULLSCREEN; 1 is merely maximized and must NOT count
expect "quiet-fs: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
dsp "hl.dsp.exec_cmd('notify-send -a q1 -t 30000 quiet body')"; sleep 1.2
chk "quiet-fs: the card landed silent, no banner" test "$(bd)" = "banners:0 resident:1"
dsp "hl.dsp.exec_cmd('notify-send -a q2 -u critical \"loud\" body')"; sleep 1.2
chk "quiet-fs: critical still punches through" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.5
dsp "hl.dsp.window.fullscreen()"; sleep 1
dsp "hl.dsp.exec_cmd('notify-send -a q3 -t 30000 loudagain body')"; sleep 1.2
chk "quiet-fs: out of fullscreen, banners are back" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; dsp "hl.dsp.window.close()"; sleep 1

# ---- inline reply -----------------------------------------------------------
# The protocol the Linux chat apps speak: a sender only offers a reply when the
# server advertises the capability, so the capability IS the feature. Driven
# entirely from the keyboard (tab arms the selected card's field, letters type,
# enter sends) and asserted on the WIRE — a card closing proves nothing on its
# own, since firing its primary closes it too.
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 40 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED" 2>&1 & )
sleep 0.5
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "Telegram" 0 "" "Alice" "are you around?" 2 "inline-reply" "Reply" 1 "category" s "im.received" 60000 >/dev/null 2>&1
sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "reply: the chat card is in the shade" test "$(st)" = "center:1 live:1 dnd:0"
tap down
tap tab
tap 35 # h
tap 23 # i
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
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- per-app policy: silenced apps, marked conversations --------------------
# DND on/off was the whole vocabulary, and "never this app" and "this person
# first" are neither of its two answers. Both rules persist, so every
# assertion is made twice: once on the live behaviour, once on the store.
# The Notify calls go over the bus with an explicit desktop-entry — the app
# key must not depend on how notify-send happens to fill the hint.
POLFILE="$STATE/hyprnotify/policy.tsv"
pol()   { hq hyprnotify policy; }
psend() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" category s "$3" 30000 >/dev/null 2>&1; }
pcrit() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 2 desktop-entry s "$1" urgency y 2 30000 >/dev/null 2>&1; }
ptran() { nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i "$1" 0 "" "$2" body 0 3 desktop-entry s "$1" category s im.received transient b true 30000 >/dev/null 2>&1; }
# the rule the hostile-file battery left behind is a REAL one, written before
# this instance existed: it is the end-to-end proof that a rule outlives the
# session that made it
chk "policy: the rule from disk survived the relaunch" test "$(pol)" = "silenced:1 s=keepme priority:0"
psend keepme "quiet please" ""; sleep 1.2
chk "policy: the persisted rule silences a fresh arrival" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50 # m
chk "policy: m lifted the persisted rule" test "$(pol)" = "silenced:0 priority:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise one" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50 # m
chk "policy: m silenced the app" test "$(pol)" = "silenced:1 s=spammer priority:0"
chk "policy: the rule reached the disk" grep -qxF "$(printf 's\tspammer')" "$POLFILE"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise two" ""; sleep 1.2
chk "policy: a silenced app's card lands with no banner" test "$(bd)" = "banners:0 resident:1"
pcrit spammer "alarm"; sleep 1.2
chk "policy: critical still punches through a silenced app" test "$(bd)" = "banners:1 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise three" ""; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 50
chk "policy: m again lifts the silence" test "$(pol)" = "silenced:0 priority:0"
chk "policy: the store emptied with it" test ! -s "$POLFILE"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.6
psend spammer "noise four" ""; sleep 1.2
chk "policy: the app banners again" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; sleep 0.6
# marking a conversation: the key is app + sender, because one chat app
# carries many people
psend chatapp Alice im.received; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 25 # p
chk "policy: p marked the sender, not the app" test "$(pol)" = "silenced:0 priority:1 p=chatapp/Alice"
tap esc; sleep 0.5
# and the mark outranks a NEWER chat from someone else. The newcomer is
# transient so it keeps its banner through the absorb — that is what tells
# the two apart in the badge after the top row is deleted.
ptran chatapp2 Bob; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "policy: the marked chat and a newer transient one" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "policy: the TOP row was the marked chat, not the newer one" test "$(bd)" = "banners:1 resident:0"
chk "policy: the mark outlives the card it was set on" test "$(pol)" = "silenced:0 priority:1 p=chatapp/Alice"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "policy: reset after the policy battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- snooze -----------------------------------------------------------------
# "Remind me": the card leaves the shade outright (Android's snooze — no
# section, nothing to scroll past) and comes back ALERTING. It is still in the
# model the whole time, which is exactly what tells a snooze apart from a
# dismissal: `state` counts it, the badge does not. snooze_seconds is turned
# down to 2 so the wake is testable at all.
sz() { hq hyprnotify snoozed; }
# `hyprctl keyword` is refused by the Lua parser ("use eval"), so the knob is
# turned the way the live config would turn it
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 2 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a later -t 60000 \"read this eventually\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "snooze: the card is up and in the shade" test "$(bd)" = "banners:0 resident:1"
tap down
tap 31 # s
chk "snooze: it left the shade" test "$(bd)" = "banners:0 resident:0"
chk "snooze: but it is still in the model, not dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "snooze: and it counts as snoozed" test "$(sz)" = 1
hq hyprnotify clear >/dev/null; sleep 0.5
chk "snooze: Clear all does not cancel a reminder" test "$(sz)" = 1
sleep 2.5
chk "snooze: it came back" test "$(sz)" = 0
chk "snooze: and it came back ALERTING, not merely parked" test "$(bd)" = "banners:1 resident:0"
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze: reset after the snooze battery" test "$(st)" = "center:0 live:0 dnd:0"

# Ranking: a critical sorts to the top however late the others arrived. The
# two cards are made TELLABLE APART in the badge — a transient one opts out of
# residency, so opening the shade absorbs the critical and leaves it a banner
# — and the keyboard deletes whatever the top row is. Both wrong answers
# (older-first, or an injector that did nothing) read differently.
dsp "hl.dsp.exec_cmd('notify-send -e -t 60000 -a chat \"an ordinary card\" body')"; sleep 0.6
dsp "hl.dsp.exec_cmd('notify-send -a alarm -u critical \"disk failing\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "ranking: an absorbed critical beside an unabsorbed transient" test "$(bd)" = "banners:1 resident:1"
tap down
tap delete
chk "ranking: the TOP row was the critical, not the card that came first" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# absorb is idempotent: toggling the center never loses or dupes a card
dsp "hl.dsp.exec_cmd('notify-send \"keep one\" body')"
dsp "hl.dsp.exec_cmd('notify-send \"keep two\" body')"; sleep 1
for i in 1 2 3; do hq hyprnotify center >/dev/null; sleep 0.35; done # on, off, on
chk "absorb: three toggles leave the two cards intact" bash -c "hyprctl -i $SIG hyprnotify state | grep -qE '^center:1 live:2 '"
hq hyprnotify center >/dev/null; sleep 0.35 # off
hq hyprnotify clear >/dev/null; sleep 0.8

# DND queues arrivals silently; the resume keeps them AND applies the same
# one-per-app cap (the resume banner assignment is now coalesce-aware — this
# guards it alongside the buildDisplay/absorb changes)
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.5
chk "DND arms" bash -c "hyprctl -i $SIG hyprnotify state | grep -q 'dnd:1'"
dsp "hl.dsp.exec_cmd('notify-send -a q one body')"
dsp "hl.dsp.exec_cmd('notify-send -a q two body')"; sleep 0.8
chk "DND: two same-app arrivals queued, none shown" test "$(bd)" = "banners:0 resident:0"
dsp "hl.plugin.hyprnotify.suspend()"; sleep 0.6
chk "DND resume: one popped, the sibling resumed resident (one per app)" test "$(bd)" = "banners:1 resident:1"
chk "DND resume: dnd off, both cards kept" bash -c "hyprctl -i $SIG hyprnotify state | grep -qE '^center:0 live:2 dnd:0$'"
hq hyprnotify clear >/dev/null; sleep 0.8

# hostile hints on the new field: a wrong-typed category must not crash the
# parse (sdbus::Error thrown + caught), the card still lands
dsp "hl.dsp.exec_cmd('notify-send -h int:category:5 \"badcat\" body')"
dsp "hl.dsp.exec_cmd('notify-send -h string:category:im.received \"convo\" body')"; sleep 1
chk "hostile: wrong-typed category survived, both cards landed" test "$(st)" = "center:0 live:2 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- real-input storm ---------------------------------------------------
{
	for i in $(seq 1 60); do echo "move $(( (i * 97) % MON_W )) $(( 30 + (i * 61) % (MON_H - 40) ))"; echo "sleep 10"; done
	for i in $(seq 1 15); do echo "move 500 13"; echo "sleep 15"; echo "scroll 0 1"; echo "sleep 25"; done
	for i in $(seq 1 10); do
		echo "move 34 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
		echo "move 59 13"; echo "sleep 15"; echo "press 272"; echo "sleep 20"; echo "release 272"; echo "sleep 35"
	done
	echo "move 12 13"; echo "sleep 30"; echo "press 272"; echo "sleep 30"; echo "release 272"; echo "sleep 100"
} | vp
sleep 1
chk "input storm: all 8 plugins alive" test "$(hq plugin list | grep -c Plugin)" = 8
chk "input storm: the final taglist click registered (ws 1)" bash -c "hyprctl -i $SIG activeworkspace -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)[\"id\"]==1 else 1)'"
printf 'move 250 200\nsleep 50\npress 272\nsleep 40\nrelease 272\nsleep 100\n' | vp
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
( { printf "move ${P% *} ${P#* }\nsleep 30\n"; for i in 1 2 3 4 5; do printf "press 272\nsleep 40\nrelease 272\nsleep 110\n"; done; } | vp ) &
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

# ---- fullscreen tuck (hyprclick) ----------------------------------------
# clicking a fullscreen window tucks a floater flagged above it (the
# compositor's pointer-focus raise sets that flag mid-viewer) back behind —
# by FLAG ONLY: its stack position must survive the viewer, or it comes
# back buried under every maximized window (reproduced live).
dsp "hl.dsp.exec_cmd('foot -a tuckmax')"; sleep 1.6
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot -a tuckfloat')"; sleep 1.6
dsp "hl.dsp.window.move({x=500, y=300})"; dsp "hl.dsp.window.resize({x=500, y=300})"; sleep 0.4
dsp "hl.dsp.exec_cmd('foot -a tuckfs -F')"; sleep 1.6
TF="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['class']=='tuckfloat'))")"
V="$(clients | python3 -c "
import json,sys
print(next(c['address'] for c in json.load(sys.stdin) if c['fullscreen']==2))")"
dsp "hl.dsp.window.alter_zorder({mode=\"top\", window=\"address:$TF\"})"; sleep 0.3
# a point the floater does not cover: the raise-click must resolve to the
# fullscreen window and fire the tuck
P="$(clients | python3 -c "
import json,sys
F=next(c for c in json.load(sys.stdin) if c['class']=='tuckfloat')
x = F['at'][0]-80 if F['at'][0] >= 110 else F['at'][0]+F['size'][0]+80
print(int(x), int(F['at'][1]+50))")"
printf "move ${P% *} ${P#* }\nsleep 30\npress 272\nsleep 40\nrelease 272\nsleep 60\n" | vp
sleep 0.6
dsp "hl.dsp.window.close({window=\"address:$V\"})"; sleep 0.9
expect "fullscreen tuck: the flagged floater survives the viewer on top" \
	"[c['class'] for c in cs if c['class'].startswith('tuck')][-1]=='tuckfloat'"
chk "fullscreen tuck: floater focused after the viewer closes" bash -c \
	"hyprctl -i $SIG activewindow -j | python3 -c 'import json,sys;sys.exit(0 if json.load(sys.stdin)[\"class\"]==\"tuckfloat\" else 1)'"
for a in $(clients | python3 -c "import json,sys;[print(c['address']) for c in json.load(sys.stdin) if c['class'].startswith('tuck')]"); do
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
