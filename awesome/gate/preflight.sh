# awesome/gate/preflight.sh — the gate's preflight: header/binary hash
# match, deploy rehearsal, launch, toast. One plugin surface: `awesome`.
echo "== gate:  =="

# ---- preflight ----------------------------------------------------------
[[ -x "$BIN" ]] || { echo "no such compositor binary: $BIN"; exit 1; }
{ [[ -x "$REPO/devtools/vptr" ]] && [[ -x "$REPO/devtools/vkbd" ]] && [[ -x "$REPO/devtools/input-capture" ]] && [[ -x "$REPO/devtools/cliphold" ]]; } || make -C "$REPO/devtools" >/dev/null
# Every pixel assertion goes through PIL. A missing python-pillow fails
# SILENTLY, not here: the capture's size check errors, the frame is dropped,
# and the launch warmup dies as a "starved render cycle" — a false symptom
# that cost a whole fork-side diagnosis (2026-10-01).
python3 -c "import PIL" 2>/dev/null && ok "python3 sees PIL (every capture assertion)" || {
	bad "python3 sees PIL (every capture assertion)"
	echo "   install python-pillow: without it every capture is dropped and the warmup starves"; exit 1
}
# The flags come from the plugin's own build (print-hl-cflags), not from a
# pkg-config call of our own: resolving it here separately is how this check
# ends up vouching for a tree nothing was built against.
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
# The headless core harness gates with the build: pure logic (stores, schema,
# boxes, wpctl parsing) that must not regress while the batteries run.
make -C "$REPO/awesome" test >/dev/null || { echo "awesome headless tests FAILED"; exit 1; }
HDR_VER=""
for d in $(make -s -C "$REPO/awesome" print-hl-cflags 2>/dev/null | tr ' ' '\n' | sed -n 's/^-I//p'); do
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
# Version lockstep: one constant (core/version.hpp); the [awesome] toml
# section must equal it once present.
AW_VER="$(grep -oE '[0-9]+\.[0-9]+\.[0-9]+' "$REPO/awesome/core/version.hpp" | head -1)"
AW_TOML="$(grep -A2 '^\[awesome\]' "$REPO/hyprpm.toml" 2>/dev/null | grep version | grep -o '[0-9.]*' | head -1)"
if [[ -n "$AW_TOML" ]]; then
	[[ "$AW_TOML" == "$AW_VER" ]] && ok "version sync (toml == version.hpp: $AW_VER)" || bad "version sync (toml=$AW_TOML vs version.hpp=$AW_VER)"
else
	[[ -n "$AW_VER" ]] && ok "version constant present (version.hpp: $AW_VER)" || bad "version constant present"
fi

# ---- build + launch -----------------------------------------------------
kill_nested
# Two variants of one tree, each with its own objects (incremental): the
# RELEASE build is the deploy rehearsal — what hyprpm builds, no test
# seams — and the GATE build (GATE=1, AWESOME_GATE seams compiled in) is
# what the nested loads. A new header set rebuilds by itself (the
# Makefile's target stamp); GATE_FORCE_BUILD=1 still forces -B.
build_aw() { # build_aw <gate 0|1> <strip PKG_CONFIG_PATH 0|1>
	local force=()
	[[ -n "${GATE_FORCE_BUILD:-}" ]] && force+=(-B)
	if [[ $2 == 1 ]]; then
		env -u PKG_CONFIG_PATH make "${force[@]}" -j"$(nproc)" -C "$REPO/awesome" GATE="$1" >/dev/null 2>&1
	else
		make "${force[@]}" -j"$(nproc)" -C "$REPO/awesome" GATE="$1" >/dev/null 2>&1
	fi
}
if [[ -n "${HYPR_DEPLOY_PKG_CONFIG_PATH:-}" ]]; then
	REHEARSAL_STRIP=0 DEPLOY_HEADERS="the explicit target pkg-config path"
else
	# hyprpm builds against its installed header cache, not this run's
	REHEARSAL_STRIP=1 DEPLOY_HEADERS="the installed header cache"
fi
build_aw 0 "$REHEARSAL_STRIP" && ok "deploy rehearsal: the release build compiles against $DEPLOY_HEADERS" || {
	bad "deploy rehearsal: the release build compiles against $DEPLOY_HEADERS"
	echo "plugin build FAILED"; exit 1
}
build_aw 1 0 && ok "the gate build (AWESOME_GATE seams) compiles" || { bad "the gate build compiles"; echo "plugin build FAILED"; exit 1; }
if [[ -n "$DEPLOY_PC_SUM" ]]; then
	chk "deploy pkg-config metadata remained untouched" test "$(sha256sum "$DEPLOY_PC_SOURCE" | cut -d' ' -f1)" = "$DEPLOY_PC_SUM"
fi
fresh_stress_state
write_stress_cfg
launch_nested || { echo "nested launch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED"; exit 1; }
ok "nested monitor is ${MON_W}x${MON_H} (every coordinate below derives from it)"
chk "the monolith loaded (one plugin, named awesome)" \
	bash -c "test \"\$(hyprctl -i '$SIG' plugin list | grep -c Plugin)\" = 1 && hyprctl -i '$SIG' plugin list | grep -q awesome"
dsp "hl.dsp.window.close()" # the donate/updated screen, when present
sleep 0.5
chk "launch toast cleared before the batteries" wait_launch_toast
