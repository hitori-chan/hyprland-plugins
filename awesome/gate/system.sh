# awesome/gate/system.sh — the system module's behavior battery: the wpctl
# process path (volume/mic), the readback caps, the repeat serialization,
# the light paths (a fake logind, backlight, keyboard LED and light sensor), a
# headset's own volume (a fake BlueZ signal), the values published to the
# control center, and the touchpad policy probe. Nothing reaches the host:
# the fake wpctl shadows the nested's PATH (the live PipeWire sink is never
# touched), and the light and headset paths talk to the nested's private
# system bus.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
ctl_part() { hq awesome controls | tr ' ' '\n' | grep "^$1:" | cut -d: -f2; } # ctl_part <name>: a published value

# the load reads every level the control center shows (the fake sink's
# 0.56, its source muted)
chk "system: the volume and the mic are published at load" test "$(ctl_part volume) $(ctl_part mic)" = "56 0"

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
# and a press with it open moves the shade's slider and posts no capsule
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: the capsule is up before the shade opens" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null; sleep 0.4
chk "system: opening the shade drops the capsule" test "$(st)" = "center:1 live:0 dnd:0"
dsp "hl.plugin.awesome.volume_up()"; sleep 0.4
chk "system: a press under the open shade moves its slider, no capsule" test "$(st)" = "center:1 live:0 dnd:0"
chk "system: ...and leaves no row (nothing on the badge)" test "$(bd)" = "banners:0 resident:0"
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
chk "headset: the readback is published to the control center" until_is 3 42 ctl_part volume
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

# ---- the keyboard light: the kernel's *kbd_backlight LED, the same path ----
KDIR="$STATE/leds/gate::kbd_backlight"
KBT() { cat "$KDIR/brightness" 2>/dev/null; }
chk "system: the fake keyboard light is seeded (100 of 200)" test "$(KBT)" = 100
chk "system: the control center learns both lights at load" test "$(ctl_part display) $(ctl_part keyboard)" = "50 50"
dsp "hl.plugin.awesome.keyboard_light_up()"
chk "system: keyboard_light_up sets +5% through logind" until_is 4 110 KBT
chk "system: the keyboard's set names the LED" grep -q "SetBrightness /org/freedesktop/login1/session/[a-z0-9]* gate::kbd_backlight 110" "$STATE/logind.log"
chk "system: it posts the percent card" until_is 3 "center:0 live:1 dnd:0" st
chk "system: ...and publishes the level" until_is 3 55 ctl_part keyboard
hq awesome clear >/dev/null; sleep 0.4
dsp "hl.plugin.awesome.keyboard_light_down()"
dsp "hl.plugin.awesome.keyboard_light_down()"
chk "system: two steps down land at 90" until_is 4 90 KBT
chk "system: the display's level never moved with it" test "$(ctl_part display)" = 50
hq awesome clear >/dev/null; sleep 0.4

# ---- the control center's block atop the shade (notify/controls.cpp) -------
# Its tiles flip the switches, a press on a level's track drags it, the
# wheel moves it 5 % a notch, the volume's glyph mutes; the values are the
# system module's, and while the shade shows them no capsule covers it.
printf '0.50\n' > "$STATE/wpctl-level" # the sink reads 50 % when the shade opens
: > "$STATE/wpctl.log"
wlog_has() { grep -Fxq "$1" "$STATE/wpctl.log" && echo 1; } # wlog_has <line>: the fake wpctl ran it
hq awesome center >/dev/null; sleep 0.8
chk "panel: the shade opens on the volume as it is now" until_is 3 50 ctl_part volume
capture_nested "$STATE/panel.png"
chk "panel: three tiles and three levels are drawn" python3 - "$STATE/panel.png" "$(tile_cx 0)" "$(tile_cx 1)" "$(tile_cx 2)" "$TILE_CY" \
	"$TRACK_X0" "$((TRACK_X0 + TRACK_W))" "$(level_cy 0)" "$(level_cy 1)" "$(level_cy 2)" <<'PY'
import sys
from PIL import Image
px = Image.open(sys.argv[1]).convert('RGB').load()
a = [int(v) for v in sys.argv[2:]]
lit = lambda x0, x1, y0, y1: sum(1 for y in range(y0, y1) for x in range(x0, x1) if max(px[x, y]) > 60)
tiles = all(lit(x - 40, x + 40, a[3] - 20, a[3] + 20) > 40 for x in a[0:3])
levels = all(lit(a[4], a[5], y - 3, y + 4) > 40 for y in a[6:9])
sys.exit(0 if tiles and levels else 1)
PY
click "$(tile_cx 0)" "$TILE_CY" 272
chk "panel: the moon's tile turns Do Not Disturb on" until_is 3 "center:1 live:0 dnd:1" st
click "$(tile_cx 0)" "$TILE_CY" 272
chk "panel: ...and off" until_is 3 "center:1 live:0 dnd:0" st
click "$(tile_cx 1)" "$TILE_CY" 272
chk "panel: the microphone's tile unmutes the muted source" until_is 3 1 wlog_has "set-mute @DEFAULT_AUDIO_SOURCE@ 0"
chk "panel: ...and its tile reads it back live" until_is 3 1 ctl_part mic
click "$(tile_cx 2)" "$TILE_CY" 272
chk "panel: no touchpad: its tile is inert" test "$(hq awesome pad)" = none
chk "panel: ...and no switch posted a capsule" test "$(st)" = "center:1 live:0 dnd:0"
# the display's row: press its knob (50 %) and drag to three quarters
printf 'move %s %s\nsleep 60\npress 272\nsleep 60\nmove %s %s\nsleep 60\nmove %s %s\nsleep 60\nrelease 272\nsleep 80\n' \
	"$(knob_x 50)" "$(level_cy 0)" "$(knob_x 60)" "$(level_cy 0)" "$(knob_x 75)" "$(level_cy 0)" | vp
chk "panel: dragging the display's row sets the backlight" until_is 4 750 BRT
chk "panel: ...through logind" grep -q "SetBrightness /org/freedesktop/login1/session/[a-z0-9]* gate0 750" "$STATE/logind.log"
chk "panel: ...and its slider holds there" until_is 3 75 ctl_part display
chk "panel: a drag posts no capsule" test "$(st)" = "center:1 live:0 dnd:0"
click "$((PANEL_X + 22))" "$(level_cy 0)" 272; sleep 0.4
chk "panel: a press on the display's glyph moves nothing" test "$(ctl_part display)" = 75
dsp "hl.plugin.awesome.keyboard_light_up()" # from the 45 % the keyboard's checks left
chk "panel: a key moves the keyboard's slider" until_is 4 50 ctl_part keyboard
chk "panel: ...and posts no capsule over it" test "$(st)" = "center:1 live:0 dnd:0"
printf 'move %s %s\nsleep 60\nscroll 0 -15\nsleep 200\n' "$(knob_x 20)" "$(level_cy 2)" | vp; sleep 0.4
chk "panel: the wheel raises the volume 5 %" until_is 3 1 wlog_has "set-volume @DEFAULT_AUDIO_SINK@ 0.55"
chk "panel: ...and its slider reads 55 back" until_is 3 55 ctl_part volume
click "$(knob_x 40)" "$(level_cy 2)" 272
chk "panel: a click on the volume's track sets it there" until_is 3 40 ctl_part volume
click "$((PANEL_X + 14 + 8))" "$(level_cy 2)" 272
chk "panel: the speaker mutes" until_is 3 1 wlog_has "set-mute @DEFAULT_AUDIO_SINK@ 1"
chk "panel: ...and the slider reads it back muted" until_is 3 40m ctl_part volume
# right on the volume: its outputs at the pointer, the default checked; a
# pick moves the default and the list reads it back
chk "panel: the outputs are read with the panel" test "$(hq awesome outputs)" = "*41 42"
click "$((PANEL_X + 60))" "$(level_cy 2)" 273
chk "panel: the volume's right click lists its outputs, the default checked" until_is 3 "Gate Speakers*;Gate Headset" hq awesome menu
click "$((PANEL_X + 120))" "$(($(level_cy 2) + 5 + 28 + 14))" 272
chk "panel: a pick makes it the default" until_is 3 1 wlog_has "set-default 42"
chk "panel: ...the outputs read it back" until_is 3 "41 *42" hq awesome outputs
chk "panel: ...and the menu closed on the pick" test -z "$(hq awesome menu)"
click "$((PANEL_X + 60))" "$(level_cy 0)" 273; sleep 0.4
chk "panel: the display's right click opens no menu" test -z "$(hq awesome menu)"
rm -f "$STATE/wpctl-level" "$STATE/wpctl-mutes" "$STATE/wpctl-default"
hq awesome center >/dev/null; sleep 0.4
dsp "hl.plugin.awesome.keyboard_light_down()"
chk "panel: closed, a key posts its capsule again" until_is 3 "center:0 live:1 dnd:0" st
hq awesome clear >/dev/null; sleep 0.4

# ---- dark displays put the keyboard out (system/light.cpp) ------------------
# Every display off takes the keyboard light with it, quietly; the first one
# back gives the level back exactly; a choice made in the dark stands.
K0="$(KBT)"
chk "dark: the keyboard is lit going in" test "${K0:-0}" -gt 0
dsp "hl.dsp.dpms({action = 'off'})"
chk "dark: the last display off puts the keyboard out" until_is 4 0 KBT
chk "dark: ...quietly (no capsule)" test "$(st)" = "center:0 live:0 dnd:0"
dsp "hl.dsp.dpms({action = 'on'})"
chk "dark: the wake gives its level back" until_is 4 "$K0" KBT
dsp "hl.dsp.dpms({action = 'off'})"
until_is 4 0 KBT >/dev/null
dsp "hl.plugin.awesome.keyboard_light_up()"
chk "dark: a key in the dark lights it" until_is 4 10 KBT
dsp "hl.dsp.dpms({action = 'on'})"; sleep 0.8
chk "dark: ...and the wake keeps that choice" test "$(KBT)" = 10
hq awesome clear >/dev/null; sleep 0.4

# ---- the keyboard light follows the room (system/light.cpp) -----------------
# Opt-in from the keyboard slider's menu. The fake sensor reads the room in
# $STATE/als (lux; seeded lit) while claimed: daylight puts the keyboard
# out, the dark lights it at the user's level (5 %, the key's in the dark
# above) once the zone held (600 ms in the gate); a flicker moves nothing;
# a choice by hand holds until the room changes; dark displays still win.
ALS="$STATE/als"
krow() { echo $(($(level_cy 1) + 5 + 14)); } # the keyboard menu's one row, under the pointer
chk "room: a light sensor is found, the room not followed" test "$(ctl_part room)" = 0
hq awesome center >/dev/null; sleep 0.8
click "$((PANEL_X + 60))" "$(level_cy 1)" 273
chk "room: the keyboard's right click offers Follow the Room" until_is 3 "Follow the Room" hq awesome menu
click "$((PANEL_X + 120))" "$(krow)" 272
chk "room: picking it follows the room" until_is 3 1 ctl_part room
chk "room: ...claiming the sensor" grep -qx "ClaimLight" "$STATE/logind.log"
chk "room: a lit room puts the keyboard out" until_is 4 0 KBT
hq awesome center >/dev/null; sleep 0.4
printf '5
' > "$ALS"
chk "room: a dark room lights it at the user's level" until_is 4 10 KBT
printf '300
' > "$ALS"; sleep 0.25; printf '5
' > "$ALS"; sleep 1.2
chk "room: a flicker shorter than the settle moves nothing" test "$(KBT)" = 10
dsp "hl.plugin.awesome.keyboard_light_down()"
chk "room: a key turns it off by hand" until_is 4 0 KBT
dsp "hl.dsp.dpms({action = 'off'})"; sleep 0.5
dsp "hl.dsp.dpms({action = 'on'})"; sleep 0.8
chk "room: ...a choice the dark room's wake leaves" test "$(KBT)" = 0
printf '300
' > "$ALS"; sleep 1.2; printf '5
' > "$ALS"
chk "room: once the room changed, its dark lights it again" until_is 4 10 KBT
dsp "hl.dsp.dpms({action = 'off'})"
chk "room: dark displays still win" until_is 4 0 KBT
dsp "hl.dsp.dpms({action = 'on'})"
chk "room: the wake lights it as the room says" until_is 4 10 KBT
hq awesome clear >/dev/null; sleep 0.4
hq awesome center >/dev/null; sleep 0.8
click "$((PANEL_X + 60))" "$(level_cy 1)" 273
chk "room: the menu checks it" until_is 3 "Follow the Room*" hq awesome menu
click "$((PANEL_X + 120))" "$(krow)" 272
chk "room: picking it again stops following" until_is 3 0 ctl_part room
chk "room: ...releasing the sensor" grep -qx "ReleaseLight" "$STATE/logind.log"
hq awesome center >/dev/null; sleep 0.4

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
