# awesome/gate/system.sh — the system module's behavior battery: the wpctl
# process path (volume/mic), the readback caps, the repeat serialization,
# the logind brightness path (a fake logind + backlight), a headset's own
# volume (a fake BlueZ signal), and the touchpad policy probe. Nothing
# reaches the host: the fake wpctl shadows the nested's PATH (the live
# PipeWire sink is never touched), and the brightness and headset paths
# talk to the nested's private system bus.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- the wpctl process path ------------------------------------------------
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: volume up uses the capped relative wpctl command" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
chk "system: volume up readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

: > "$STATE/flood-wpctl"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.5
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
dsp "hl.plugin.awesome.volume_down()"; sleep 0.4
chk "system: volume down uses the relative wpctl command" grep -Fxq "set-volume @DEFAULT_AUDIO_SINK@ 5%-" "$STATE/wpctl.log"
chk "system: volume down readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

: > "$STATE/wpctl.log"
dsp "(function() for _ = 1, 32 do hl.plugin.awesome.volume_up() end return hl.dsp.no_op() end)()"; sleep 0.8
# one set in flight per device: the first press runs, the other 31
# accumulate (capped at 20 steps) into ONE more set — never 32 racing
# read-modify-writes inside wpctl
burst_serialized() {
	[[ "$(grep -c '^set-volume ' "$STATE/wpctl.log")" == 2 ]] &&
		[[ "$(grep '^set-volume ' "$STATE/wpctl.log" | tail -1)" == "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 100%+" ]]
}
chk "system: a repeat burst serializes into one accumulated set" burst_serialized
chk "system: the capped chains still produced one card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

# a level already past 100% (set elsewhere) is never pulled down to it: the
# first press reads 1.50 back, the second limits at that level, not at 1.0
printf '1.50\n' > "$STATE/wpctl-level"
: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: volume up keeps a level past 100%" grep -Fxq "set-volume -l 1.505 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
rm -f "$STATE/wpctl-level"
hq awesome clear >/dev/null; sleep 0.4

# an OSD is never a shade row: opening the shade drops the live capsule,
# and a press with it open shows a capsule over it (top center, clear of
# the column) that leaves no row behind
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: the capsule is up before the shade opens" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null; sleep 0.4
chk "system: opening the shade drops the capsule" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: a press under the open shade shows its capsule" test "$(st)" = "center:1 live:1 dnd:0"
chk "system: the capsule is no row (nothing on the badge)" test "$(bd)" = "banners:0 resident:0"
hq awesome center >/dev/null; sleep 0.4
nclear
chk "system: the shade closes clean" test "$(st)" = "center:0 live:0 dnd:0"

# a Bluetooth headset's own volume (AVRCP absolute volume): no key reaches
# the binds; BlueZ announces the transport's Volume on the system bus, and
# the capsule follows the sink's readback — only when something moved
bt_volume() { # bt_volume <0..127> [own] — a transport's PropertiesChanged, as org.bluez when "own"
	DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" python3 - "$1" "${2:-own}" <<'PY'
import sys
from gi.repository import Gio, GLib
bus = Gio.DBusConnection.new_for_address_sync(
    __import__("os").environ["DBUS_SYSTEM_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
if sys.argv[2] == "own":
    bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                  GLib.Variant("(su)", ("org.bluez", 4)), GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 2000, None)
bus.emit_signal(None, "/org/bluez/hci0/dev_00_00_00_00_00_01/sep1/fd0", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                GLib.Variant("(sa{sv}as)", ("org.bluez.MediaTransport1", {"Volume": GLib.Variant("q", int(sys.argv[1]))}, [])))
bus.flush_sync(None)
PY
}
printf '0.42\n' > "$STATE/wpctl-level"
: > "$STATE/wpctl.log"
bt_volume 53
chk "headset: its own volume change shows the capsule" until_is 3 "center:0 live:1 dnd:0" st
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
rm -f "$STATE/wpctl-level"
nclear

: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.mute()"; sleep 0.4
chk "system: mute uses the sink toggle" grep -Fxq "set-mute @DEFAULT_AUDIO_SINK@ toggle" "$STATE/wpctl.log"
chk "system: the mute readback posts a volume card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.plugin.awesome.mute()"; sleep 0.4
chk "system: the second mute readback replaces it (one fixed-id card)" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

: > "$STATE/wpctl.log"
dsp "hl.plugin.awesome.mic_mute()"; sleep 0.4
chk "system: mic mute uses the source toggle" grep -Fxq "set-mute @DEFAULT_AUDIO_SOURCE@ toggle" "$STATE/wpctl.log"
chk "system: the mic readback posts a card" test "$(st)" = "center:0 live:1 dnd:0"
dsp "hl.plugin.awesome.mic_mute()"; sleep 0.4
hq awesome clear >/dev/null; sleep 0.4

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
dsp "hl.plugin.awesome.brightness_up()"; sleep 0.4
BAFTER_UP="$(brt_wait up "$BSTART")"
chk "system: brightness_up sets +5% through logind" test "$BAFTER_UP" = 550
chk "system: the set went through the logind session path" grep -q "SetBrightness /org/freedesktop/login1/session/[a-z0-9]* gate0 550" "$STATE/logind.log"
chk "system: brightness_up posts the percent card on logind's ack" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4
dsp "hl.plugin.awesome.brightness_down()"; sleep 0.4
BAFTER_DOWN="$(brt_wait down "$BAFTER_UP")"
chk "system: brightness_down moved it back" test "$BAFTER_DOWN" = 500
chk "system: brightness_down posts the percent card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

# ---- the touchpad policy ------------------------------------------------------
PSTART="$(hq awesome pad)"
case "$PSTART" in
none)
	chk "system: no touchpad device — the policy reports none" test "$PSTART" = none
	;;
*)
	# Two toggles always return the live touchpad to the state the battery
	# found it in; the state string flipping between them is the behavior.
	PFLIP="$( [[ "$PSTART" == on ]] && echo off || echo on )"
	dsp "hl.plugin.awesome.touchpad_toggle()"; sleep 0.5
	chk "system: the toggle flipped the live touchpad ($PSTART -> $PFLIP)" test "$(hq awesome pad)" = "$PFLIP"
	chk "system: the toggle posts the state card" test "$(st)" = "center:0 live:1 dnd:0"
	hq awesome clear >/dev/null; sleep 0.4
	dsp "hl.plugin.awesome.touchpad_toggle()"; sleep 0.5
	chk "system: the second toggle restores the start state" test "$(hq awesome pad)" = "$PSTART"
	hq awesome clear >/dev/null; sleep 0.4
	;;
esac

chk "system: final clean state" test "$(st)" = "center:0 live:0 dnd:0"
