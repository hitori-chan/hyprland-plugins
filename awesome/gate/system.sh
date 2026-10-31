# awesome/gate/system.sh — the system module's behavior battery: the wpctl
# process path (volume/mic), the readback caps, the repeat serialization,
# the backlight (a fake logind and backlight), a headset's own volume (a
# fake bluetoothd) and the touchpad policy. Nothing reaches the host: the
# fake wpctl shadows the nested's PATH, and the backlight and headset paths
# talk to the nested's private system bus.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
# idle: every wpctl set has run and been read back (a readback's card is
# posted in the same dispatch that retires it)
idle() { until_is 5 0 hq awesome busy; }

# ---- the wpctl process path ------------------------------------------------
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"; idle
chk "system: volume up uses the capped relative wpctl command" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
chk "system: volume up readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null

: > "$STATE/flood-wpctl"
dsp "hl.plugin.awesome.volume_up()"; idle
flood_stopped() { # the reader closed the pipe, or killed the producer
	local pid
	[[ -s "$STATE/flood-wpctl.closed" ]] && return 0
	pid="$(cat "$STATE/flood-wpctl.pid" 2>/dev/null)"
	[[ "$pid" =~ ^[1-9][0-9]*$ ]] && ! kill -0 "$pid" 2>/dev/null
}
chk "system: flood readback stops the producer at the retained-output cap" flood_stopped
chk "system: flood readback emits no guessed feedback" test "$(st)" = "center:0 live:0 dnd:0"
chk "system: flood leaves the nested compositor responsive" hq_matches '^center:0 live:0 dnd:0$' awesome state
rm -f "$STATE/flood-wpctl" "$STATE/flood-wpctl.closed" "$STATE/flood-wpctl.pid"

: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_down()"; idle
chk "system: volume down uses the relative wpctl command" grep -Fxq "set-volume @DEFAULT_AUDIO_SINK@ 5%-" "$STATE/wpctl.log"
chk "system: volume down readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null

: > "$STATE/wpctl.log"
dsp "(function() for _ = 1, 32 do hl.plugin.awesome.volume_up() end return hl.dsp.no_op() end)()"; idle
# one set in flight per device: the first press runs, the other 31
# accumulate (capped at 20 steps) into ONE more set — never 32 racing
# read-modify-writes inside wpctl
burst_serialized() {
	[[ "$(grep -c '^set-volume ' "$STATE/wpctl.log")" == 2 ]] &&
		[[ "$(grep '^set-volume ' "$STATE/wpctl.log" | tail -1)" == "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 100%+" ]]
}
chk "system: a repeat burst serializes into one accumulated set" burst_serialized
chk "system: the capped chains still produced one card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null

# a level already past 100% (set elsewhere) is never pulled down to it: the
# first press reads 1.50 back, the second limits at that level, not at 1.0.
# Elsewhere is later than the last readback's trust (system/audio.cpp's
# LEVEL_TRUST, 1 s): within it a raise limits at the level it read.
printf '1.50\n' > "$STATE/wpctl-level"
sleep 1.1
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"; idle
dsp "hl.plugin.awesome.volume_up()"; idle
chk "system: volume up keeps a level past 100%" grep -Fxq "set-volume -l 1.505 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
rm -f "$STATE/wpctl-level"
hq awesome clear >/dev/null

# an OSD is never a shade row: opening the shade drops the live capsule
dsp "hl.plugin.awesome.volume_up()"; idle
chk "system: the capsule is up before the shade opens" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null
chk "system: opening the shade drops the capsule" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome center >/dev/null
chk "system: the shade closes clean" until_is 3 "center:0 live:0 dnd:0" st

# a Bluetooth headset's own volume (AVRCP absolute volume): no key reaches
# the binds; BlueZ announces the transport's Volume on the system bus, and
# the capsule follows the sink's readback — only when something moved
sbus() { DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" busctl --system "$@"; }
bt_volume() { # bt_volume <0..127> [other] — a transport's PropertiesChanged from the fake bluetoothd, or a stranger
	if [[ "${2:-own}" == own ]]; then
		sbus call org.bluez / org.awesome.Gate EmitVolume q "$1" >/dev/null
		return
	fi
	DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" python3 - "$1" <<'PY'
import os, sys
from gi.repository import Gio, GLib
bus = Gio.DBusConnection.new_for_address_sync(
    os.environ["DBUS_SYSTEM_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
bus.emit_signal(None, "/org/bluez/hci0/dev_00_00_00_00_00_01/sep1/fd0", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                GLib.Variant("(sa{sv}as)", ("org.bluez.MediaTransport1", {"Volume": GLib.Variant("q", int(sys.argv[1]))}, [])))
bus.flush_sync(None)
PY
}
printf '0.42\n' > "$STATE/wpctl-level"
: > "$STATE/wpctl.log"
bt_volume 53
chk "headset: its own volume change shows the capsule" until_is 3 "center:0 live:1 dnd:0" st
chk "headset: ...at the sink's level" until_is 3 "Volume 42%" hq awesome capsule
chk "headset: the capsule came from a readback, not a set" bash -c "grep -Fxq 'get-volume @DEFAULT_AUDIO_SINK@' '$STATE/wpctl.log' && ! grep -q '^set-' '$STATE/wpctl.log'"
nclear
bt_volume 53
sleep 0.6
chk "headset: an announcement that moved nothing shows nothing" test "$(st)" = "center:0 live:0 dnd:0"
printf '0.37\n' > "$STATE/wpctl-level"
bt_volume 47 other
sleep 0.6
chk "headset: a sender that is not BlueZ moves nothing" test "$(st)" = "center:0 live:0 dnd:0"
bt_volume 47
chk "headset: the next real change shows again" until_is 3 "center:0 live:1 dnd:0" st
nclear
# PipeWire takes an A2DP headset's level 200 ms after the announcement (the
# fake's .next file): read inside that, every swipe's capsule showed the
# swipe before it
printf '0.52\n' > "$STATE/wpctl-level.next"
bt_volume 66
chk "headset: the capsule shows the level PipeWire took, not the one before" until_is 3 "Volume 52%" hq awesome capsule
rm -f "$STATE/wpctl-level" "$STATE/wpctl-level.next"
nclear

: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.mute()"; idle
chk "system: mute uses the sink toggle" grep -Fxq "set-mute @DEFAULT_AUDIO_SINK@ toggle" "$STATE/wpctl.log"
chk "system: the mute readback posts a volume card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.plugin.awesome.mute()"; idle
chk "system: the second mute readback replaces it (one fixed-id card)" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null

: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.mic_mute()"; idle
chk "system: mic mute uses the source toggle" grep -Fxq "set-mute @DEFAULT_AUDIO_SOURCE@ toggle" "$STATE/wpctl.log"
chk "system: the mic readback posts a card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.plugin.awesome.mic_mute()"; idle
hq awesome clear >/dev/null

# ---- the logind brightness path --------------------------------------------
# The plugin's real path — sysfs read, logind SetBrightness over the system
# bus — against the nested's PRIVATE system bus: a fake logind writes the
# fake backlight the harness points the plugin at (AW_BACKLIGHT_DIR,
# seeded 500/1000 per launch). The host's panel never moves.
BDIR="$STATE/backlight/gate0"
BRT() { cat "$BDIR/brightness" 2>/dev/null; }
brt_wait() { # brt_wait <up|down> <base> — poll until the fake backlight moved
	local dir=$1 base=$2 got
	for _ in $(seq 1 40); do
		got="$(BRT)"
		[[ "$dir" == up && "$got" -gt "$base" ]] && { echo "$got"; return 0; }
		[[ "$dir" == down && "$got" -lt "$base" ]] && { echo "$got"; return 0; }
		sleep 0.1
	done
	echo "$base"
	return 1
}
BSTART="$(BRT)"
chk "system: the fake backlight is seeded (500 of 1000)" test "$BSTART" = 500
dsp "hl.plugin.awesome.brightness_up()"
BAFTER_UP="$(brt_wait up "$BSTART")"
chk "system: brightness_up sets +5% through logind" test "$BAFTER_UP" = 550
chk "system: the set went through the logind session path" grep -q "SetBrightness /org/freedesktop/login1/session/[a-z0-9]* gate0 550" "$STATE/logind.log"
chk "system: brightness_up posts the percent card on logind's ack" until_is 3 "center:0 live:1 dnd:0" st
hq awesome clear >/dev/null
dsp "hl.plugin.awesome.brightness_down()"
BAFTER_DOWN="$(brt_wait down "$BAFTER_UP")"
chk "system: brightness_down moved it back" test "$BAFTER_DOWN" = 500
chk "system: brightness_down posts the percent card" until_is 3 "center:0 live:1 dnd:0" st
hq awesome clear >/dev/null

# ---- the touchpad (system/pad.cpp) -------------------------------------------
# The nested has no libinput device: a second virtual pointer stands in
# (the gate build's `pad NAME`). hl.device records a flip and applies it on
# a scheduled input refresh, so the state is the device config's, not the
# lagging attachment. Its key answers with a capsule. The stand-in ends on:
# its config outlives it, and every virtual pointer shares its name.
chk "touchpad: none without one" test "$(hq awesome pad)" = none
mice() { hq devices -j | python3 -c 'import json,sys;[print(m["name"]) for m in json.load(sys.stdin)["mice"]]' | sort; }
M0="$(mice)"
exec {PADFD}> >(WAYLAND_DISPLAY="$WL" "$REPO/devtools/vptr" "$MON_W" "$MON_H" >/dev/null 2>&1)
standin() { comm -13 <(echo "$M0") <(mice) | head -1; }
has_standin() { [[ -n "$(standin)" ]]; }
until_ok 3 has_standin
PADN="$(standin)"
chk "touchpad: a virtual pointer stands in" test -n "$PADN"
hq awesome pad "$PADN" >/dev/null
chk "touchpad: the stand-in reads on" test "$(hq awesome pad)" = on
dsp "hl.plugin.awesome.touchpad_toggle()"
chk "touchpad: the key turns it off" until_is 3 off hq awesome pad
chk "touchpad: ...with its capsule" until_is 3 "center:0 live:1 dnd:0" st
hq awesome clear >/dev/null
dsp "hl.plugin.awesome.touchpad_toggle()"
chk "touchpad: the key turns it back on" until_is 3 on hq awesome pad
hq awesome clear >/dev/null
hq awesome pad - >/dev/null
exec {PADFD}>&-
chk "touchpad: the stand-in leaves" until_is 3 none hq awesome pad

chk "system: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
