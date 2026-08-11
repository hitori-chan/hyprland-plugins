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
if [[ -n "$DEPLOY_PC_SUM" ]]; then
	chk "deploy pkg-config metadata remained untouched" test "$(sha256sum "$DEPLOY_PC_SOURCE" | cut -d' ' -f1)" = "$DEPLOY_PC_SUM"
fi
rm -rf "$STATE"; mkdir -p "$STATE/hyprplace"
printf '100\t100\t500\t400\tfoot\n200\t80\tlegacyfoot\n' > "$STATE/hyprplace/lastspot.tsv"
{
	echo 'hl.config({ ecosystem = { enforce_permissions = true } })'
	echo 'hl.permission(".*hyprland-plugins/.*", "plugin", "allow")'
	echo 'hl.permission(".*input-capture$", "input-capture", "allow")'
	echo 'hl.permission(".*vkbd$", "keyboard", "allow")'
	echo 'hl.permission(".*grim$", "screencopy", "allow")'
	cat "$HARNESS/nested.lua"
	echo 'hl.window_rule({ match = { class = "foot|mpv|corpseA|corpseB|tuckmax|tuckfloat|tuckfs" }, float = true })'
} > "$CFG"
launch_nested || { echo "nested launch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED"; exit 1; }
LOG="$HARNESS/nested.log"
ok "nested monitor is ${MON_W}x${MON_H} (every coordinate below derives from it)"
chk "8 plugins loaded" test "$(hq plugin list | grep -c Plugin)" = 8
dsp "hl.dsp.window.close()" # the donate/updated screen, when present
sleep 0.5
