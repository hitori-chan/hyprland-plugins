#!/usr/bin/env bash
# devtools/stress.sh — the pre-deploy regression gate. Boots the full plugin
# stack in the nested harness and drives it through the storm battery:
# placement memory, sibling geometry, spawn/close storms, the notification
# cap, state churn round-trips, hostile state files, a real-input storm
# (vptr), the shade's click and key verbs (vkbd), the bell's click path, the
# below-shade OSD hitbox while the shade is open, hyprosd's wpctl process
# sequence, the click-corpse guard, acting-closes-the-shade, the fullscreen
# tuck and a config reload. Every assertion is exact; any failure fails the run.
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
CAPTURE_LOG="$HARNESS/input-capture.log"
RUNDIR="${XDG_RUNTIME_DIR:?}/hypr"
SIG=""
CAPTURE_PID=""

PASS=0
FAILED=()
ok()  { PASS=$((PASS + 1)); printf '  ok  %s\n' "$1"; }
bad() { FAILED+=("$1"); printf ' FAIL %s\n' "$1"; }
chk() { # chk <name> <command...> — command's exit code decides
	local name=$1; shift
	if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
}

normalize_target_pkgconfig() {
	local pkg_path=${HYPR_DEPLOY_PKG_CONFIG_PATH:-}
	[[ -n "$pkg_path" ]] || return 0

	# The deploy path is intentionally one disposable package set. A colon
	# list could make pkg-config select a different hyprland.pc than the one
	# normalized here.
	if [[ "$pkg_path" == *:* ]]; then
		echo "HYPR_DEPLOY_PKG_CONFIG_PATH must name one pkg-config directory" >&2
		return 1
	fi

	local pc="$pkg_path/hyprland.pc"
	[[ -f "$pc" ]] || {
		echo "missing hyprland.pc under HYPR_DEPLOY_PKG_CONFIG_PATH: $pkg_path" >&2
		return 1
	}

	local prefix
	prefix="$(cd "$pkg_path/../.." 2>/dev/null && pwd)/include" || return 1
	[[ -d "$prefix/hyprland" ]] || {
		echo "missing target headers beside HYPR_DEPLOY_PKG_CONFIG_PATH: $prefix" >&2
		return 1
	}

	# CMake writes the configured install prefix into hyprland.pc even when
	# cmake --install is redirected to a disposable --prefix. Fix that one
	# generated line before common.mk resolves the plugin flags.
	sed -i "s|^prefix=.*|prefix=$prefix|" "$pc"
}

hq()      { hyprctl -i "$SIG" "$@"; }
dsp()     { hq dispatch "$1" >/dev/null 2>&1; }
clients() { hq clients -j 2>/dev/null; }
ws()      { hq activeworkspace -j | python3 -c 'import json,sys;print(json.load(sys.stdin)["id"])'; }
reserved() { hq monitors -j | python3 -c 'import json,sys;print(",".join(map(str,json.load(sys.stdin)[0]["reserved"])))'; }

# Nothing here may hard-code the nested monitor's size: it is whatever window
# the Wayland backend gets, and it can change when the nested config or its
# host surface is applied. retarget re-reads the instance after every launch
# and vp injects through vptr with that real extent — vptr maps coordinates as
# X/extent onto the output, so a stale extent silently lands every scripted
# click somewhere else and the assertion passes or fails on whatever happened
# to be under it.
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

stop_capture() {
	if [[ -n "$CAPTURE_PID" ]]; then
		kill "$CAPTURE_PID" 2>/dev/null || true
		wait "$CAPTURE_PID" 2>/dev/null || true
		CAPTURE_PID=""
	fi
}

kill_nested() { # kill any non-live instance running one of the harness cfgs
	stop_capture
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

launch_nested() {
	PATH="$REPO/devtools/fakes:$PATH" HYPROSD_WPCTL_LOG="$STATE/wpctl.log" \
		HYPROSD_WPCTL_HANG_FILE="$STATE/hang-wpctl" HYPRNOTIFY_SOUND_HANG_FILE="$STATE/hang-sound" \
		HYPR_BIN="$BIN" HYPR_CFG="$CFG" XDG_STATE_HOME="$STATE" \
		bash "$HARNESS/launch.sh" >/dev/null 2>&1
}

echo "== stress: $BIN =="

# ---- preflight ----------------------------------------------------------
[[ -x "$BIN" ]] || { echo "no such compositor binary: $BIN"; exit 1; }
{ [[ -x "$REPO/devtools/vptr" ]] && [[ -x "$REPO/devtools/vkbd" ]] && [[ -x "$REPO/devtools/input-capture" ]]; } || make -C "$REPO/devtools" >/dev/null
# The headers the plugins compile against must belong to the gated binary —
# a scratch hyprland.pc keeps its absolute /usr/local prefix (not
# relocatable), silently falls back to the installed tree, and every plugin
# embeds the wrong hash: all 8 mismatch-throw at load. Normalize the scratch
# pc's prefix= to its own include/ before gating a fork build.
#
# The flags come from common.mk, not from a pkg-config call of our own:
# resolving it here separately is how this check ends up vouching for a tree
# nothing was built against (a distro hyprland package in /usr next to the
# fork in /usr/local is enough).
normalize_target_pkgconfig || exit 1
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" && "${PKG_CONFIG_PATH:-}" != "$HYPR_DEPLOY_PKG_CONFIG_PATH" ]]; then
	echo "PKG_CONFIG_PATH and HYPR_DEPLOY_PKG_CONFIG_PATH must name the same target package set" >&2
	exit 1
fi
HDR_VER=""
for d in $(make -s -C "$REPO/hyprnotify" print-hl-cflags 2>/dev/null | tr ' ' '\n' | sed -n 's/^-I//p'); do
	for v in "$d/hyprland/src/version.h" "$d/src/version.h"; do
		[[ -f "$v" ]] && { HDR_VER="$v"; break 2; }
	done
done
HDR_ROOT="${HDR_VER%/hyprland/src/version.h}"
HDR_HASH="$(grep -h GIT_COMMIT_HASH "$HDR_VER" 2>/dev/null | grep -oE '[0-9a-f]{40}')"
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
# deploy rehearsal FIRST: hyprpm builds against ITS OWN cached headers, not
# this run's scratch set — a plugin that cannot build there bricks the whole
# hyprpm swap (hyprplace 2.0.1 did). Dropping PKG_CONFIG_PATH leaves
# common.mk's own default, the installed compositor's headers. For an
# uninstalled fork, HYPR_DEPLOY_PKG_CONFIG_PATH points at the fork's disposable
# scratch package set so this rehearsal checks the target that will actually
# run. These throwaway builds are overwritten just below.
J="-j$(nproc)"
dep_ok=1
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" ]]; then
	DEPLOY_ENV=(env PKG_CONFIG_PATH="$HYPR_DEPLOY_PKG_CONFIG_PATH")
	DEPLOY_HEADERS="the explicit target pkg-config path"
else
	DEPLOY_ENV=(env -u PKG_CONFIG_PATH)
	DEPLOY_HEADERS="the installed header cache"
fi
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
	"${DEPLOY_ENV[@]}" make -B "$J" -C "$REPO/$p" >/dev/null 2>&1 || { dep_ok=0; echo "  deploy-build broke: $p"; }
done
[[ $dep_ok == 1 ]] && ok "deploy rehearsal: all 8 build against $DEPLOY_HEADERS" || bad "deploy rehearsal build"
# now the real builds for this run's compositor (caller's PKG_CONFIG_PATH)
build_ok=1
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
	make -B "$J" -C "$REPO/$p" >/dev/null 2>&1 || { build_ok=0; echo "  build broke: $p"; }
done
[[ $build_ok == 1 ]] && ok "all 8 plugins build" || { echo "plugin build FAILED"; exit 1; }
rm -rf "$STATE"; mkdir -p "$STATE/hyprplace"
printf '100\t100\t500\t400\tfoot\n200\t80\tlegacyfoot\n' > "$STATE/hyprplace/lastspot.tsv"
{
	echo 'hl.config({ ecosystem = { enforce_permissions = true } })'
	echo 'hl.permission(".*hyprland-plugins/.*", "plugin", "allow")'
	echo 'hl.permission(".*input-capture$", "input-capture", "allow")'
	echo 'hl.permission(".*vkbd$", "keyboard", "allow")'
	cat "$HARNESS/nested.lua"
	echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs" }, float = true })'
} > "$CFG"
launch_nested || { echo "nested launch FAILED"; exit 1; }
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
# The spot is occupied, not retired: free it and check the next sibling's
# result. On a small nested output the remaining 500x400 sibling may still
# overlap the remembered box; in that case the least-overlap fallback is the
# correct result, and the test must not demand an impossible free placement.
B="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at'] != [100,100]), ''))")"
A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot' and c['at']==[100,100]), ''))")"
dsp "hl.dsp.window.close({window=\"address:$A\"})"
# The close dispatch is asynchronous. Do not let the next spawn race the
# unmap and mistake the still-mapped owner for a permanently occupied spot.
for _ in $(seq 1 30); do
	LEFT="$(clients | python3 -c "
import json,sys
address = sys.argv[1]
print(any(c['address'] == address for c in json.load(sys.stdin)))" "$A")"
	[[ "$LEFT" != 1 ]] && break
	sleep 0.1
done
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
placementReclaim() {
	clients | python3 -c "
import json,sys
cs = json.load(sys.stdin)
b = next((c for c in cs if c['address'] == '$B'), None)
def overlaps(c):
    return c['at'][0] < 600 and c['at'][0] + c['size'][0] > 100 and c['at'][1] < 500 and c['at'][1] + c['size'][1] > 100
on_spot = any(c['class'] == 'foot' and c['at'] == [100,100] and c['size'] == [500,400] for c in cs)
positions = {tuple(c['at']) for c in cs if c['class'] == 'foot'}
print(1 if b and len(positions) == 2 and ((not overlaps(b) and on_spot) or (overlaps(b) and not on_spot)) else 0)"
}
chk "freed spot reuse respects remaining sibling geometry" test "$(placementReclaim)" = 1

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
# A plugin-maximized window lives outside the compositor fullscreen model.
# Changing a native reserved area must therefore reach hyprmax explicitly.
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
MAX_BEFORE="$(box)"
RESERVED_BEFORE="$(reserved)"
# The Wayland backend can queue a startup overlay before this probe. Force a
# wrapped replacement so creation changes the reservation even when an error
# bar already exists at the same output edge.
ERROR_MESSAGE="reserved-area-probe $(printf 'wrapped-message-with-enough-width-to-force-a-second-line %.0s' {1..6})"
hq seterror 'rgba(ff3030ff)' "$ERROR_MESSAGE" >/dev/null 2>&1
for _ in $(seq 1 30); do
	RESERVED_WITH_ERROR="$(reserved)"
	[[ "$RESERVED_WITH_ERROR" != "$RESERVED_BEFORE" ]] && break
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
	ok "hyprmax: native reserved-area change reflows maximized geometry"
else
	bad "hyprmax: native reserved-area change reflows maximized geometry"
fi
hq seterror disable >/dev/null 2>&1
for _ in $(seq 1 30); do
	[[ "$(reserved)" == "$RESERVED_BEFORE" ]] && break
	sleep 0.1
done
chk "fork: disabling error overlay restores native reserved area" test "$(reserved)" = "$RESERVED_BEFORE"
for _ in $(seq 1 30); do
	[[ "$(box)" == "$MAX_BEFORE" ]] && break
	sleep 0.1
done
chk "hyprmax: removing native reservation restores maximized workarea" test "$(box)" = "$MAX_BEFORE"
dsp "hl.plugin.hyprmax.toggle()"; sleep 0.5
chk "hyprmax: reserved-area roundtrip preserves windowed restore" test "$(box)" = "$REF"
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
launch_nested || { echo "relaunch FAILED"; exit 1; }
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
# Drawing it (the placement break plus the paging cue) must not crash and
# must keep every card.
for i in $(seq 1 15); do dsp "hl.dsp.exec_cmd('notify-send -a ovf$i -t 30000 \"row $i\" body')"; done; sleep 1.5
hq hyprnotify center >/dev/null; sleep 0.6
chk "overflow: a 15-item center renders paged, keeps every card" test "$(st)" = "center:1 live:15 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- hardening: the shade's click model, absorb, DND, hostile hints -----
# A compact shade row reveals its hidden content before its body can act;
# once open, its BODY fires the card's primary and dismisses it. Exercise the
# exact merged-chat case through the real hit boxes via vptr. The panel hangs
# off the monitor's right edge
# (EDGE 10 + CENTER_W 360) below offset_y 34, so the first row's body is a
# fixed inset from the top-right corner, and the chevron rides the row's
# right end (ROW_PADX 12 + CHEV 24). Hit boxes are final-position, so the
# open spring cannot move them out from under the click. Three Telegram-style
# messages merge into one row: after the chevron compacts it, the first body
# click must expand and preserve it; only the second, now-open body click may
# dismiss the actionless card.
for m in first second third; do
	dsp "hl.dsp.exec_cmd('notify-send -a Telegram -c im.received -t 30000 Alice \"$m\"')"
	sleep 0.3
done
sleep 0.8
chk "shade: Telegram-style messages merge into one card" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.6
	click() { # click <x> <y> <button-code>
		printf 'move %s %s\nsleep 40\npress %s\nsleep 40\nrelease %s\nsleep 80\n' "$1" "$2" "$3" "$3" |
			vp
		sleep 0.8
	}
	longpress() { # longpress <x> <y> <button-code>
		printf 'move %s %s\nsleep 80\npress %s\nsleep 650\nrelease %s\nsleep 120\n' "$1" "$2" "$3" "$3" |
			vp
		sleep 0.8
	}
ROWX=$((MON_W - 10 - 360 + 10 + 80)) # panel x + body pad + into the text column
ROWY=64                              # offset_y + body pad + into the first row
CHVX=$((MON_W - 10 - 10 - 12 - 12))  # panel right edge - body pad - ROW_PADX - half CHEV
click $CHVX $ROWY 272
chk "shade: the chevron compacts the merged card without acting" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "shade: compact merged-card body expands before acting" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
# notify-send carries no actions, so the now-open body click is a pure
# DISMISSAL, and dismissing never closes the shade (center stays 1). The
# battery below is the other half: a card that does fire.
chk "shade: open actionless BODY dismisses it, shade stays" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"right me\" body')"; sleep 1
click $ROWX $ROWY 273
chk "shade: right on a row dismisses it" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "hardening: reset after the shade click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- acting CLOSES the shade (Android's collapse-on-click) ------------------
# Firing a card's primary raises the sender over the very panel the click was
# made in, so the panel leaves with it — AOSP collapses the shade on a
# content-intent click, swaync ships the same as hide-on-action. `resident`
# is the fd.o way of saying the action does NOT take you away, and it holds
# the shade exactly as it holds the card. The actionless card above is the
# third case: nothing fired, so that click was only a dismissal.
#
# NOT notify-send, even though -A can send the action: notify-send WAITS for
# its action and EXITS the moment one fires, and libnotify closes the
# notification on the way out. The card then dies down the bus
# CloseNotification path rather than from the click, so "did the click keep
# the card?" would be measuring notify-send. busctl's call returns and
# leaves nothing behind — the card's whole life is hyprnotify's.
nfy() { # nfy <summary> <a{sv} hints...> — a card carrying a real `default`
	local sum="$1"; shift
	DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify 'susssasa{sv}i' gatechat 0 "" "$sum" body 2 default Open "$@" 30000 >/dev/null
}
nfy "open me" 0; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the shade is open with the firing card in it" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: the primary took the card AND the shade with it" test "$(st)" = "center:0 live:0 dnd:0"
nfy "stay me" 1 resident b true; sleep 1
hq hyprnotify center >/dev/null; sleep 0.6
chk "close-on-act: the resident card is in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
click $ROWX $ROWY 272
chk "close-on-act: a resident card's action keeps card and shade both" test "$(st)" = "center:1 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8
chk "close-on-act: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

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

# ---- the bell's click path --------------------------------------------------
# The bell has no hover action. Its private bus interface exposes only the
# click-equivalent Toggle, which opens a real shade and absorbs the popped
# banner like any other explicit center open.
nbus() { DBUS_SESSION_BUS_ADDRESS="$NBUS" busctl --user "$@"; }
chk "bell: the removed hover method is absent" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; ! nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify | grep -q '\.Peek'"
dsp "hl.dsp.exec_cmd('notify-send -t 30000 \"bell click\" body')"; sleep 1
chk "bell: a banner is up and the shade is shut" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify Toggle >/dev/null 2>&1; sleep 0.5
chk "bell: click opens the shade" test "$(st)" = "center:1 live:1 dnd:0"
chk "bell: click absorbs the banner" test "$(bd)" = "banners:0 resident:1"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.hitori.hyprnotify Toggle >/dev/null 2>&1; sleep 0.4
chk "bell: second click closes the shade" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
chk "bell: reset after the click battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- hyprosd wpctl process path -------------------------------------------
# The nested compositor shadows only wpctl, so this reaches the real Lua,
# deferred queue, pidfd, pipe readback, and notification paths without changing
# the live PipeWire sink. The fake validates argv and emits real wpctl output.
: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_up()"; sleep 0.4
chk "hyprosd: volume up uses the capped relative wpctl command" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
chk "hyprosd: volume up readback produces an OSD card" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_down()"; sleep 0.4
chk "hyprosd: volume down uses the relative wpctl command" grep -Fxq "set-volume @DEFAULT_AUDIO_SINK@ 5%-" "$STATE/wpctl.log"
chk "hyprosd: volume down readback produces an OSD card" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

: > "$STATE/wpctl.log"
dsp "(function() for _ = 1, 32 do hl.plugin.hyprosd.volume_up() end return hl.dsp.no_op() end)()"; sleep 0.8
chk "hyprosd: repeat backpressure caps active chains" test "$(grep -c '^set-volume ' "$STATE/wpctl.log")" = 16
chk "hyprosd: repeat backpressure preserves admitted feedback" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

# ---- OSD below an open shade ----------------------------------------------
# OSD-band cards are deliberately absent from shade rows and the bell badge,
# but their active surface must remain visible below a shade that was already
# open. Send the same fixed-id/value shape as hyprosd's brightness path, then
# click its popup hitbox. A working below-shade card dismisses while leaving
# the shade open; a hidden card leaves the OSD in the model or hits the panel.
hq hyprnotify center >/dev/null; sleep 0.5
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i osd 9992 "" Brightness 72% 0 3 value i 72 urgency y 0 x-hitori-osd b true 1200 >/dev/null 2>&1
sleep 0.35
chk "center OSD: brightness card is active" test "$(st)" = "center:1 live:1 dnd:0"
chk "center OSD: fixed card remains outside shade accounting" test "$(bd)" = "banners:0 resident:0"
OSD_PANEL_H=$((10 + 46 + 10 + 4 + 34 + 12))
OSD_Y=$((34 + OSD_PANEL_H + 6 + 14))
click "$((MON_W - 10 - 348 / 2))" "$OSD_Y" 272
chk "center OSD: below-shade popup hitbox dismisses the card" test "$(st)" = "center:1 live:0 dnd:0"
sleep 1.4
chk "center OSD: card expires without closing the shade" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4

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
# enter is the body click's twin, so it collapses for the same reason: "key
# one" carries a real -A default, and firing it raises the sender over the
# panel. The card goes AND the shade goes with it.
chk "keys: enter fires the primary, taking the card and the shade" test "$(st)" = "center:0 live:0 dnd:0"
# which leaves esc nothing to close — give it its own shade, or the line
# below passes on a shade that was already gone
hq hyprnotify center >/dev/null; sleep 0.5
chk "keys: a fresh shade for esc to close" test "$(st)" = "center:1 live:0 dnd:0"
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
# a silence carries its expiry as a third field; 0 is the one that never lifts
chk "policy: the rule reached the disk" grep -qxF "$(printf 's\tspammer\t0')" "$POLFILE"
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

# ---- the manage panel, timed mutes, the rule count --------------------------
# Android exposes notification management through a press-and-hold gesture. The
# shade keeps the chevron solely for expansion; long-press turns the target into
# a full-width labelled manage panel. Driven through REAL hit boxes: the press
# starts on the row/digest body, and entries stack below the panel header at
# MENU_ROW_H each.
ent() { echo $((44 + 9 + 28 + $1 * 28 + 14)); }  # ent <index> -> that entry's centre y
ENTX=$((MON_W - 200))                            # anywhere in the entry's width
# the battery above deliberately leaves a MARK standing (it outlives its card),
# so these read the silence half alone rather than the whole line
polsil() { hq hyprnotify policy | sed 's/ priority:.*//'; }
psend mgr "manage me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
longpress $ROWX $ROWY 272
chk "manage: long-press opened the panel — nothing acted, nothing dismissed" test "$(st)" = "center:1 live:1 dnd:0"
chk "manage: opening it set no rule" test "$(polsil)" = "silenced:0"
click $ENTX "$(ent 2)" 272 # "Mute mgr for 1 hour"
# A timed rule prints its seconds remaining, and that is a clock read — assert
# the SHAPE (timed at all, and near the hour asked for), never the exact value.
chk "manage: a timed mute landed WITH an expiry" bash -c "[[ '$(polsil)' =~ ^silenced:1[[:space:]]s=mgr\\+(35[0-9]{2}|3600)$ ]]"
chk "manage: the expiry reached the disk" grep -qE "^s	mgr	[0-9]{10}$" "$POLFILE"
psend mgr "still muted" ""; sleep 1.2
chk "manage: a timed rule silences an arrival exactly as a permanent one does" test "$(bd)" = "banners:0 resident:2"
# and the way back out: silenced, the panel offers Unmute where the three
# durations were, so a rule is never one you cannot find the end of
longpress $ROWX $ROWY 272
click $ENTX "$(ent 2)" 272 # "Unmute mgr"
chk "manage: the panel lifted the rule" test "$(polsil)" = "silenced:0"
chk "manage: and no silence is left on disk" bash -c "! grep -q '^s	' '$POLFILE'"
tap esc
hq hyprnotify clear >/dev/null; sleep 0.8
for i in 1 2 3 4; do psend bundle "bundle $i" ""; sleep 0.2; done; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
chk "manage/group: four same-app cards form one digest" test "$(st)" = "center:1 live:4 dnd:0"
longpress $ROWX $ROWY 272
chk "manage/group: long-press opened the bundle panel" test "$(st)" = "center:1 live:4 dnd:0"
click $ENTX "$(ent 0)" 272 # "Mute bundle for 1 hour"
chk "manage/group: timed bundle mute landed WITH an expiry" bash -c "[[ '$(polsil)' =~ ^silenced:1[[:space:]]s=bundle\+(35[0-9]{2}|3600)$ ]]"
longpress $ROWX $ROWY 272
click $ENTX "$(ent 0)" 272 # "Unmute bundle"
chk "manage/group: bundle panel lifted the rule" test "$(polsil)" = "silenced:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "manage: reset after the manage battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- the undo window behind a snooze ----------------------------------------
# Snooze used to be the one verb with no recourse. The card now holds its slot
# as an undo row for a few seconds, so `snoozed` goes up and comes back DOWN
# without the card ever having left the model.
sz() { hq hyprnotify snoozed; }
# 12s: comfortably past the 6s undo window so the lapse is testable without
# racing the wake (8s left under a second of margin, and the key injection
# alone spends most of that), yet short enough that the card wakes and can be
# swept before the next battery. A snoozed card is unreachable by design —
# Clear all does not cancel a reminder — so letting it wake is the ONLY way to
# leave this battery clean.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 12 } } })' >/dev/null; sleep 0.4
psend undoer "take it back" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 31 # s
chk "undo: s snoozed the card" test "$(sz)" = 1
tap 22 # u
chk "undo: u took the snooze back" test "$(sz)" = 0
chk "undo: and the card never left the model" test "$(st)" = "center:1 live:1 dnd:0"
tap 31 # s again — and this time let the window lapse
chk "undo: snoozed again" test "$(sz)" = 1
sleep 7
chk "undo: the window lapsed and the snooze stands" test "$(sz)" = 1
tap 22 # u past the window is not an undo — there is no row to have clicked
chk "undo: u past the window does nothing" test "$(sz)" = 1
sleep 6
chk "undo: and it still woke on its own clock" test "$(sz)" = 0
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "undo: reset after the undo battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- swipe: the horizontal wheel on a row -----------------------------------
# An ADDITION on top of the pointer path — a mouse without a horizontal wheel
# must lose no verb — so this asserts the gesture works, not that it is the
# only way. Away dismisses; back opens the manage panel.
swipe() { printf 'move %s %s\nsleep 60\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 30\nscroll 1 %s\nsleep 200\n' "$1" "$2" "$3" "$3" "$3" | vp; sleep 0.9; }
psend swiper "flick me" ""; sleep 1.2
hq hyprnotify center >/dev/null; sleep 0.7
chk "swipe: a card in an open shade" test "$(st)" = "center:1 live:1 dnd:0"
swipe "$ROWX" "$ROWY" -25
chk "swipe: back opened the manage panel, it did not dismiss" test "$(st)" = "center:1 live:1 dnd:0"
# esc peels the panel, not the shade. This is the assertion that the peel
# order is right even though the panel was gesture-opened.
tap esc
chk "swipe: esc peeled the panel and left the shade up" test "$(st)" = "center:1 live:1 dnd:0"
swipe "$ROWX" "$ROWY" 25
chk "swipe: away dismissed the row" test "$(st)" = "center:1 live:0 dnd:0"
tap esc; hq hyprnotify clear >/dev/null; sleep 0.8
chk "swipe: reset after the swipe battery" test "$(st)" = "center:0 live:0 dnd:0"

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

# A snoozed chat whose sender keeps talking must STAY away. The conversation
# merge deliberately aims a chat's new messages at the one card that holds it,
# and a replace re-alerts by design — so without an explicit exception the
# snooze ended at the next thing the sender said, which is the one case it
# exists for. The card must take the message and no banner with it.
hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 5 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a tgz -c im.received -t 60000 Zoe first')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
tap down
tap 31 # s
chk "snooze/merge: the chat card is away" test "$(sz)" = 1
dsp "hl.dsp.exec_cmd('notify-send -a tgz -c im.received -t 60000 Zoe second')"; sleep 1.2
chk "snooze/merge: a new message does not wake it" test "$(sz)" = 1
chk "snooze/merge: and it took no banner" test "$(bd)" = "banners:0 resident:0"
chk "snooze/merge: still one card, not two" test "$(st)" = "center:1 live:1 dnd:0"
sleep 6
chk "snooze/merge: the wake still arrives on its own clock" test "$(bd)" = "banners:1 resident:0"
tap esc; hq eval 'hl.config({ plugin = { hyprnotify = { snooze_seconds = 900 } } })' >/dev/null
hq hyprnotify clear >/dev/null; sleep 0.8
chk "snooze/merge: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

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

# Retained notification state is deliberately bounded. The transport has
# already copied this payload, but the model must parse only its first body
# cap, retain four thumbnails and twelve action pairs, and stay responsive.
FLOOD="$({ for i in $(seq 1 32); do printf '<img src="/no-such-image/%s" alt="image">&' "$i"; done; head -c 20000 /dev/zero | tr '\0' '&'; })"
FLOOD_ACTIONS=()
for i in $(seq 1 24); do
	FLOOD_ACTIONS+=("action-$i" "Action $i")
done
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i flood 0 "" flood "$FLOOD" "${#FLOOD_ACTIONS[@]}" "${FLOOD_ACTIONS[@]}" 0 30000 >/dev/null 2>&1
sleep 1
chk "admission: hostile markup/actions/images leave one responsive card" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- native input capture ------------------------------------------------
# The protocol is fed after plugin input listeners emit. A receiver that gets
# all three event classes proves the bar, shade, click, max, and snap gates
# did not cancel or swallow an event owned by an active capture session.
CAPTURE_READY=0
WAYLAND_DISPLAY="$WL" timeout 12s "$REPO/devtools/input-capture" "$MON_W" "$MON_H" >"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!
for _ in $(seq 1 50); do
	if grep -qx 'READY' "$CAPTURE_LOG" 2>/dev/null; then
		CAPTURE_READY=1
		break
	fi
	if ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
		break
	fi
	sleep 0.1
done
chk "input capture: receiver reached READY" test "$CAPTURE_READY" = 1
if [[ $CAPTURE_READY == 1 ]]; then
	{
		echo "move $((MON_W / 2)) $((MON_H / 2))"
		echo "sleep 60"
		# Absolute motion is a warp and does not cross input-capture barriers;
		# use native relative motion for the edge crossing.
		echo "rel 0 -$((MON_H / 2 + 40))"
		echo "sleep 120"
		echo "move $((MON_W / 2 + 40)) $((MON_H / 2))"
		echo "sleep 80"
		echo "press 272"
		echo "sleep 40"
		echo "release 272"
		echo "sleep 80"
	} | vp
	# Creating vkbd after activation replaces the compositor's active keymap;
	# this exercises the fork's EIS keyboard-device restart while captured.
	printf 'tap a\nsleep 100\n' | vk
	wait "$CAPTURE_PID"; CAPTURE_STATUS=$?
	CAPTURE_PID=""
	chk "input capture: motion, button, and key events reached EIS" test "$CAPTURE_STATUS" = 0
else
	stop_capture
fi
sleep 0.5

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
# The placement store intentionally remembers the same class geometry. Move
# these two viewers apart after spawn so the dying viewer always has an
# exposed corner to receive the tail of the click burst.
dsp "hl.dsp.window.move({x=$((MON_W / 10)), y=$((MON_H / 6))})"; sleep 0.4
dsp "hl.dsp.exec_cmd('foot -a corpseB')"; sleep 1.6
dsp "hl.dsp.window.move({x=$((MON_W / 2)), y=$((MON_H / 8))})"; sleep 0.4
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

# ---- config reload ------------------------------------------------------
# hyprbar and hyprnotify both re-resolve their disk-loaded icons from
# config.reloaded: the bar re-probes its icon dirs, drops every resolved
# texture, marks the live tray items dirty and warms again. Nothing may
# wedge, and the strip must still take a click afterwards.
#
# retarget FIRST, and not only for the signature: the Wayland backend can hand
# the instance a different window size before nested.lua's monitor line and a
# reload can apply another output size. vptr maps `move X Y` as X/extent, so
# every coordinate below is off by that ratio until the extent is re-read.
chk "reload: the config re-applies" bash -c "hyprctl -i $SIG reload | grep -q ok"
sleep 1.2
retarget
chk "reload: all 8 plugins alive" test "$(hq plugin list | grep -c Plugin)" = 8
chk "reload: hyprnotify still answers" bash -c "hyprctl -i $SIG hyprnotify state | grep -q '^center:'"
printf "move 59 13\nsleep 30\npress 272\nsleep 30\nrelease 272\nsleep 120\n" | vp; sleep 0.6
chk "reload: the strip still takes a click (tag 3)" test "$(ws)" = 3
printf "move 12 13\nsleep 30\npress 272\nsleep 30\nrelease 272\nsleep 120\n" | vp; sleep 0.6
chk "reload: and back on tag 1" test "$(ws)" = 1

# ---- log hygiene --------------------------------------------------------
chk "log clean (only known-benign lines)" bash -c \
	"! grep -iE 'error|assert|segv|abort' '$LOG' | grep -vE 'Creating the Error Overlay|xkbcomp' | grep -q ."

# ---- teardown -----------------------------------------------------------
# A mapped plugin cannot retain callbacks into its code after unload, but an
# external helper also cannot be allowed to hold compositor exit hostage.
# Start one helper through each owner, require bounded shutdown, then remove
# the deliberately hung fixtures owned by this test.
: > "$STATE/hang-wpctl"
: > "$STATE/hang-sound"
dsp "hl.plugin.hyprosd.volume_up()"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i teardown 0 "" "hung sound" body 0 1 sound-name s gate 30000 >/dev/null 2>&1
for _ in $(seq 1 30); do
	[[ -s "$STATE/hang-wpctl.pid" && -s "$STATE/hang-sound.pid" ]] && break
	sleep 0.1
done
chk "teardown: hyprosd owns an active helper" test -s "$STATE/hang-wpctl.pid"
chk "teardown: hyprnotify owns an active helper" test -s "$STATE/hang-sound.pid"
NESTED_PID="$(head -1 "$RUNDIR/$SIG/hyprland.lock" 2>/dev/null)"
chk "teardown: nested compositor pid is known" test -n "$NESTED_PID"
kill_nested
for _ in $(seq 1 30); do
	! kill -0 "$NESTED_PID" 2>/dev/null && break
	sleep 0.1
done
if [[ -n "$NESTED_PID" ]] && ! kill -0 "$NESTED_PID" 2>/dev/null; then
	ok "teardown: active helpers do not block compositor exit"
else
	bad "teardown: active helpers do not block compositor exit"
	[[ -n "$NESTED_PID" ]] && kill -KILL "$NESTED_PID" 2>/dev/null || true
fi
for marker in "$STATE/hang-wpctl" "$STATE/hang-sound"; do
	pid="$(cat "${marker}.pid" 2>/dev/null)"
	[[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
done
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
