# ffi — the Rust "awesome" probe (cabi C ABI). It is loaded for the WHOLE run
# (nested.lua lists it last, so it never reorders the C++ plugins' input
# priority table) and is the first non-C++ code to cross the fork's C boundary.
#
# What this battery asserts:
#   1. The loader took the C path — "awesome" is in the plugin list with the
#      metadata the probe wrote through the out-params (name/version/desc are
#      C strings the fork copied; a wrong ABI or a broken handshake would drop
#      the plugin or mangle them).
#   2. The probe survives config reloads. A reload re-applies the config while
#      the probe's dispatchers/jobs stay armed; a dangling callback into the
#      plugin (or a job firing mid-reload) would crash the nested or drop the
#      plugin from the list.
# The clean-unload side (hyprPluginExitC cancels jobs + clears listeners before
# the ctx is dropped) is covered by the lifecycle battery's teardown core check.

# 1. present + metadata round-trip -----------------------------------------
chk "ffi: Rust probe is in the plugin list" test "$(hq plugin list | grep -c 'Plugin awesome')" = 1
chk "ffi: Rust probe reports its C-ABI version (1.0.0)" \
    test "$(hq plugin list | grep -A3 'Plugin awesome' | grep -c 'Version: 1.0.0')" = 1
chk "ffi: Rust probe metadata round-trips (cabi description)" \
    test "$(hq plugin list | grep -A3 'Plugin awesome' | grep -c 'cabi')" = 1

# 2. survive config reloads -------------------------------------------------
for i in 1 2 3; do
	chk "ffi: config reload #$i re-applies" hq_matches 'ok' reload
	sleep 1.5
	chk "ffi: Rust probe survives config reload #$i" \
	    test "$(hq plugin list | grep -c 'Plugin awesome')" = 1
	chk "ffi: all $NPLUGINS plugins alive after reload #$i" \
	    test "$(hq plugin list | grep -c Plugin)" = "$NPLUGINS"
done
