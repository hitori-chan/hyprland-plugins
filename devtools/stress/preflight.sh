echo "== stress: $BIN =="

# ---- preflight ----------------------------------------------------------
[[ -x "$BIN" ]] || { echo "no such compositor binary: $BIN"; exit 1; }
{ [[ -x "$REPO/devtools/vptr" ]] && [[ -x "$REPO/devtools/vkbd" ]] && [[ -x "$REPO/devtools/input-capture" ]] && [[ -x "$REPO/devtools/cliphold" ]]; } || make -C "$REPO/devtools" >/dev/null
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
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" && "${PKG_CONFIG_PATH:-}" != "$HYPR_DEPLOY_PKG_CONFIG_PATH" ]]; then
	echo "PKG_CONFIG_PATH and HYPR_DEPLOY_PKG_CONFIG_PATH must name the same target package set" >&2
	exit 1
fi
DEPLOY_PC_SOURCE="${HYPR_DEPLOY_PKG_CONFIG_PATH:-}/hyprland.pc"
DEPLOY_PC_SUM=""
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" ]]; then
	DEPLOY_PC_SUM="$(sha256sum "$DEPLOY_PC_SOURCE" | cut -d' ' -f1)"
fi
normalize_target_pkgconfig || exit 1
make -C "$REPO/devtools" test >/dev/null || { echo "standalone regressions FAILED"; exit 1; }
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
# this run's scratch set. Dropping PKG_CONFIG_PATH leaves
# common.mk's own default, the installed compositor's headers. For an
# uninstalled fork, HYPR_DEPLOY_PKG_CONFIG_PATH points at the fork's disposable
# scratch package set so this rehearsal checks the target that will actually
# run. These throwaway builds are overwritten just below.
# With an explicit target set, rehearsal and gate builds are provably
# identical (path equality is enforced, normalization retargets both onto one
# scratch pc): build once, credit both assertions. Only the installed-cache
# fallback case needs two different passes.
# 8 sequential -B passes took ~95s of the gate; one slot per plugin keeps
# the total job count at nproc. PJ is the per-plugin -j under that cap.
NP="$(nproc)"
PJ="-j$(( (NP + 7) / 8 ))"
build_all() { # $1: 1 = strip PKG_CONFIG_PATH (installed-cache rehearsal)
	local strip=$1 p pid ok=1
	local -A who=()
	for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
		if [[ $strip == 1 ]]; then
			( env -u PKG_CONFIG_PATH make -B "$PJ" -C "$REPO/$p" ) >/dev/null 2>&1 &
		else
			( make -B "$PJ" -C "$REPO/$p" ) >/dev/null 2>&1 &
		fi
		who[$!]=$p
	done
	for pid in "${!who[@]}"; do
		wait "$pid" || { ok=0; echo "  deploy-build broke: ${who[$pid]}"; }
	done
	return $(( 1 - ok ))
}
build_ok=1
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" ]]; then
	build_all 0 || build_ok=0
	if [[ $build_ok == 1 ]]; then
		ok "deploy rehearsal: all 8 build against the explicit target pkg-config path"
		ok "all 8 plugins build"
	else
		bad "deploy rehearsal build"
		bad "all 8 plugins build"
		echo "plugin build FAILED"; exit 1
	fi
else
	DEPLOY_HEADERS="the installed header cache"
	# The rehearsal strips PKG_CONFIG_PATH; the gate build uses this shell's
	# environment. When both resolve to the SAME cflags (the installed fork
	# is the target), the two -B passes are byte-identical — one build
	# credits both, exactly as the explicit-target branch does. A different
	# resolved target (a distro tree in /usr next to the fork) pays both.
	REH_CFLAGS="$(env -u PKG_CONFIG_PATH make -s -C "$REPO/hyprnotify" print-hl-cflags 2>/dev/null)"
	GATE_CFLAGS="$(make -s -C "$REPO/hyprnotify" print-hl-cflags 2>/dev/null)"
	if [[ -n "$REH_CFLAGS" && "$REH_CFLAGS" == "$GATE_CFLAGS" ]]; then
		build_all 1 || { bad "deploy rehearsal build"; bad "all 8 plugins build"; echo "plugin build FAILED"; exit 1; }
		ok "deploy rehearsal: all 8 build against $DEPLOY_HEADERS (identical to the target flags; one build credits both)"
		ok "all 8 plugins build"
	else
		build_all 1 && ok "deploy rehearsal: all 8 build against $DEPLOY_HEADERS" || bad "deploy rehearsal build"
		build_all 0 && ok "all 8 plugins build" || { echo "plugin build FAILED"; exit 1; }
	fi
fi
if [[ -n "$DEPLOY_PC_SUM" ]]; then
	chk "deploy pkg-config metadata remained untouched" test "$(sha256sum "$DEPLOY_PC_SOURCE" | cut -d' ' -f1)" = "$DEPLOY_PC_SUM"
fi
fresh_stress_state
write_stress_cfg
launch_nested || { echo "nested launch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED"; exit 1; }
LOG="$HARNESS/nested.log"
ok "nested monitor is ${MON_W}x${MON_H} (every coordinate below derives from it)"
chk "8 plugins loaded" test "$(hq plugin list | grep -c Plugin)" = 8
dsp "hl.dsp.window.close()" # the donate/updated screen, when present
sleep 0.5
chk "launch toast cleared before the batteries" wait_launch_toast
