# awesome/gate/system.sh — the system module's behavior battery: the wpctl
# process path (volume/mic), the readback caps, the repeat serialization,
# the light paths (a fake logind, backlight, keyboard LED and light sensor), a
# headset's own volume (a fake BlueZ signal), the values published to the
# control center, and the touchpad's switch. Nothing reaches the host:
# the fake wpctl shadows the nested's PATH (the live PipeWire sink is never
# touched), and the light and headset paths talk to the nested's private
# system bus.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
ctl_part() { hq awesome controls | tr ' ' '\n' | grep "^$1:" | cut -d: -f2; } # ctl_part <name>: a published value
# idle: every wpctl set has run and been read back (a readback's card is
# posted in the same dispatch that retires it)
idle() { until_is 5 0 hq awesome busy; }

# the load reads every level the control center shows (the fake sink's
# 0.56, its source muted)
chk "system: the volume and the mic are published at load" test "$(ctl_part volume) $(ctl_part mic)" = "56 0"

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

# an OSD is never a shade row: opening the shade drops the live capsule;
# with the control center open, a press moves its slider and posts none
dsp "hl.plugin.awesome.volume_up()"; idle
chk "system: the capsule is up before the shade opens" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome center >/dev/null
chk "system: opening the shade drops the capsule" test "$(st)" = "center:1 live:0 dnd:0"
hq awesome controlcenter >/dev/null
panels() { echo "$(st) $(hq awesome controlcenter-shown)"; } # the center's state, then the control center's
chk "system: the control center takes the shade's place in the column" until_is 3 "center:0 live:0 dnd:0 1" panels
dsp "hl.plugin.awesome.volume_up()"; idle
chk "system: a press under the open control center moves its slider, no capsule" test "$(st)" = "center:0 live:0 dnd:0"
chk "system: ...and leaves no row (nothing on the badge)" test "$(bd)" = "banners:0 resident:0"
hq awesome controlcenter >/dev/null
nclear
chk "system: the panels close clean" until_is 3 "center:0 live:0 dnd:0 0" panels

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

# ---- Bluetooth (system/bluetooth.cpp) --------------------------------------
# The fake bluetoothd (devtools/fakes/fake-sysbus): hci0 powered, Gate Buds
# (headphones, 80 %) and Gate Mouse (connected) paired, a stranger seen but
# never paired. The panel's doors are driven by name through the gate's
# verb; bluetoothd's own signals carry every change back.
chk "bt: the paired devices, connected first, the stranger left out" until_is 3 "on Gate Mouse=c Gate Buds=n80" hq awesome bt
: > "$STATE/logind.log"
hq awesome bt connect Gate Buds >/dev/null
chk "bt: a connect goes to bluetoothd" until_ok 3 grep -qx "Connect Gate Buds" "$STATE/logind.log"
chk "bt: ...and lands connected, by name among the connected" until_is 3 "on Gate Buds=c80 Gate Mouse=c" hq awesome bt
sbus call org.bluez / org.awesome.Gate SetBattery sy "Gate Buds" 35 >/dev/null
chk "bt: a battery moves with bluetoothd's signal" until_is 3 "on Gate Buds=c35 Gate Mouse=c" hq awesome bt
hq awesome bt power 0 >/dev/null
chk "bt: power off drops every connection" until_is 3 "off Gate Buds=n35 Gate Mouse=n" hq awesome bt
: > "$STATE/logind.log"
hq awesome bt connect Gate Mouse >/dev/null
chk "bt: a device picked while off powers the radio, then connects" until_is 3 "on Gate Mouse=c Gate Buds=n35" hq awesome bt
chk "bt: ...in that order" bash -c "tr '\n' '|' < '$STATE/logind.log' | grep -q '^Powered 1|Connect Gate Mouse|'"
hq awesome bt disconnect Gate Mouse >/dev/null
chk "bt: a disconnect" until_is 3 "on Gate Buds=n35 Gate Mouse=n" hq awesome bt

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
hq awesome clear >/dev/null
dsp "hl.plugin.awesome.keyboard_light_down()"
dsp "hl.plugin.awesome.keyboard_light_down()"
chk "system: two steps down land at 90" until_is 4 90 KBT
chk "system: the display's level never moved with it" test "$(ctl_part display)" = 50
hq awesome clear >/dev/null; sleep 0.4

# ---- the control center (notify/controls.cpp) -------------------------------
# The bar's switches open it (shell.sh clicks them; here the verb): its
# buttons flip the switches, a press on a level's track drags it, the wheel
# moves it 5 % a notch, the volume's low glyph mutes, its corner lists the
# outputs; the values are the system module's, and while the panel shows
# them no capsule covers it.
printf '0.50\n' > "$STATE/wpctl-level" # the sink reads 50 % when the panel opens
: > "$STATE/wpctl.log"
wlog_has() { grep -Fxq "$1" "$STATE/wpctl.log" && echo 1; } # wlog_has <line>: the fake wpctl ran it
hq awesome controlcenter >/dev/null
chk "panel: the control center opens" until_is 3 1 hq awesome controlcenter-shown
chk "panel: it opens on the volume as it is now" until_is 3 50 ctl_part volume
capture_nested "$STATE/panel.png"
chk "panel: its first row and three level modules are drawn" python3 - "$STATE/panel.png" "$DND_CX" "$(cc_btn_cx 0)" "$(cc_btn_cx 1)" "$CC_CY" \
	"$TRACK_X0" "$(level_cy 0)" "$(level_cy 1)" "$(level_cy 2)" <<'PY'
import sys
from PIL import Image
px = Image.open(sys.argv[1]).convert('RGB').load()
a = [int(v) for v in sys.argv[2:]]
lit = lambda x0, x1, y0, y1: sum(1 for y in range(y0, y1) for x in range(x0, x1) if max(px[x, y]) > 60)
row = all(lit(x - 24, x + 24, a[3] - 24, a[3] + 24) > 40 for x in a[0:3])
levels = all(lit(a[4], a[4] + 100, y - 3, y + 4) > 40 for y in a[5:8])
sys.exit(0 if row and levels else 1)
PY
click "$DND_CX" "$CC_CY" 272
chk "panel: Do Not Disturb's capsule turns it on" until_is 3 "center:0 live:0 dnd:1" st
click "$DND_CX" "$CC_CY" 272
chk "panel: ...and off" until_is 3 "center:0 live:0 dnd:0" st
click "$(cc_btn_cx 0)" "$CC_CY" 272
chk "panel: the microphone's button unmutes the muted source" until_is 3 1 wlog_has "set-mute @DEFAULT_AUDIO_SOURCE@ 0"
chk "panel: ...and reads it back live" until_is 3 1 ctl_part mic
click "$(cc_btn_cx 1)" "$CC_CY" 272
chk "panel: no touchpad: its button is inert" test "$(hq awesome pad)" = none
chk "panel: ...and no button posted a capsule" test "$(st)" = "center:0 live:0 dnd:0"
# the display's: press its track at its level (50 %) and drag to three quarters
printf 'move %s %s\nsleep 60\npress 272\nsleep 60\nmove %s %s\nsleep 60\nmove %s %s\nsleep 60\nrelease 272\nsleep 80\n' \
	"$(track_x 0 50)" "$(level_cy 0)" "$(track_x 0 60)" "$(level_cy 0)" "$(track_x 0 75)" "$(level_cy 0)" | vp
chk "panel: dragging the display's slider sets the backlight" until_is 4 750 BRT
chk "panel: ...through logind" grep -q "SetBrightness /org/freedesktop/login1/session/[a-z0-9]* gate0 750" "$STATE/logind.log"
chk "panel: ...and its slider holds there" until_is 3 75 ctl_part display
chk "panel: a drag posts no capsule" test "$(st)" = "center:0 live:0 dnd:0"
click "$LOW_X" "$(level_cy 0)" 272
click "$(track_x 0 20)" "$(($(mod_y 0) + 20))" 272; sleep 0.4
chk "panel: a press on the display's glyph or over its track's name row moves nothing" test "$(ctl_part display)" = 75
printf 'move %s %s\nsleep 60\nscroll 0 -15\nsleep 200\n' "$(track_x 0 20)" "$(level_cy 0)" | vp
chk "panel: the wheel steps the display as its key does" until_is 4 800 BRT
dsp "hl.plugin.awesome.keyboard_light_up()" # from the 45 % the keyboard's checks left
chk "panel: a key moves the keyboard's slider" until_is 4 50 ctl_part keyboard
chk "panel: ...and posts no capsule over it" test "$(st)" = "center:0 live:0 dnd:0"
printf 'move %s %s\nsleep 60\nscroll 0 -15\nsleep 200\n' "$(track_x 2 20)" "$(level_cy 2)" | vp
chk "panel: the wheel raises the volume as its key does" until_is 3 1 wlog_has "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+"
chk "panel: ...and its slider reads 55 back" until_is 3 55 ctl_part volume
click "$(track_x 2 40)" "$(level_cy 2)" 272
chk "panel: a click on the volume's track sets it there" until_is 3 40 ctl_part volume
click "$LOW_X" "$(level_cy 2)" 272
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
click "$CORNER_CX" "$(corner_cy 2)" 272
chk "panel: the volume's corner lists the outputs too" until_is 3 "Gate Speakers;Gate Headset*" hq awesome menu
tap esc
cc_menu() { echo "$(hq awesome controlcenter-shown) $(hq awesome menu)"; } # "<panel shown> <menu rows>"
chk "panel: ...and Esc closes its menu, the panel stays" until_is 3 "1 " cc_menu
click "$((PANEL_X + 60))" "$(level_cy 0)" 273
chk "panel: the display's right click opens no menu" test -z "$(hq awesome menu)"
rm -f "$STATE/wpctl-level" "$STATE/wpctl-mutes" "$STATE/wpctl-default"

# A light this coarse (a ThinkPad's keyboard: off, low, high) is a stepped
# slider: a press lands on the nearest level. The panel opening finds a
# light again once its device stopped reading.
KPCT="$(ctl_part keyboard)"
hq awesome controlcenter >/dev/null
chk "panel: the verb closes it" until_is 3 0 hq awesome controlcenter-shown
mv "$KDIR" "$STATE/leds/gone"
K3="$STATE/leds/three::kbd_backlight"
mkdir -p "$K3" && printf '2\n' > "$K3/max_brightness" && printf '1\n' > "$K3/brightness"
hq awesome controlcenter >/dev/null
chk "steps: the opening panel finds the three-level light, at Low" until_is 3 50 ctl_part keyboard
click "$(track_x 1 80)" "$(level_cy 1)" 272
chk "steps: a press at four fifths lands on High" until_is 3 2 cat "$K3/brightness"
chk "steps: ...and the slider reads it full" until_is 3 100 ctl_part keyboard
click "$(track_x 1 30)" "$(level_cy 1)" 272
chk "steps: a press at three tenths lands on Low" until_is 3 1 cat "$K3/brightness"
click "$(track_x 1 10)" "$(level_cy 1)" 272
chk "steps: a press at a tenth puts it out" until_is 3 0 cat "$K3/brightness"
hq awesome controlcenter >/dev/null
rm -rf "$K3" && mv "$STATE/leds/gone" "$KDIR"
hq awesome controlcenter >/dev/null
chk "steps: the panel finds the old light again" until_is 3 "$KPCT" ctl_part keyboard
hq awesome controlcenter >/dev/null
dsp "hl.plugin.awesome.keyboard_light_down()"
chk "panel: closed, a key posts its capsule again" until_is 3 "center:0 live:1 dnd:0" st
hq awesome clear >/dev/null

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
# Opt-in from the keyboard slider's menu or its module's corner button. The fake sensor reads the room in
# $STATE/als (lux; seeded lit) while claimed: daylight puts the keyboard
# out, the dark lights it at the user's level (5 %, the key's in the dark
# above) once the zone held (600 ms in the gate); a flicker moves nothing;
# a choice by hand holds until the room changes; dark displays still win.
ALS="$STATE/als"
krow() { echo $(($(level_cy 1) + 5 + 14)); } # the keyboard menu's one row, under the pointer
chk "room: a light sensor is found, the room not followed" test "$(ctl_part room)" = 0
hq awesome controlcenter >/dev/null
click "$((PANEL_X + 60))" "$(level_cy 1)" 273
chk "room: the keyboard's right click offers Follow the Room" until_is 3 "Follow the Room" hq awesome menu
click "$((PANEL_X + 120))" "$(krow)" 272
chk "room: picking it follows the room" until_is 3 1 ctl_part room
chk "room: ...claiming the sensor" grep -qx "ClaimLight" "$STATE/logind.log"
chk "room: a lit room puts the keyboard out" until_is 4 0 KBT
hq awesome controlcenter >/dev/null
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
hq awesome controlcenter >/dev/null
click "$((PANEL_X + 60))" "$(level_cy 1)" 273
chk "room: the menu checks it" until_is 3 "Follow the Room*" hq awesome menu
click "$((PANEL_X + 120))" "$(krow)" 272
chk "room: picking it again stops following" until_is 3 0 ctl_part room
chk "room: ...releasing the sensor" grep -qx "ReleaseLight" "$STATE/logind.log"
click "$CORNER_CX" "$(corner_cy 1)" 272
chk "room: the keyboard's corner button follows it too" until_is 3 1 ctl_part room
click "$CORNER_CX" "$(corner_cy 1)" 272
chk "room: ...and stops" until_is 3 0 ctl_part room
hq awesome controlcenter >/dev/null

# ---- the touchpad (system/pad.cpp) -------------------------------------------
# The nested has no libinput device: a second virtual pointer stands in
# (the gate build's `pad NAME`). hl.device records a flip and applies it on
# a scheduled input refresh, so the state is the device config's, not the
# lagging attachment: the switch shows a flip at once, and its next press
# flips it back (it asked for the state the touchpad was already in). The
# click's own virtual pointer leaving is no hotplug: the flip outlives the
# re-check it used to arm (400 ms). The stand-in ends on: its config
# outlives it, and every virtual pointer shares its name.
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
hq awesome controlcenter >/dev/null
chk "touchpad: the switch reads it on" until_is 3 1 ctl_part touchpad
click "$(cc_btn_cx 1)" "$CC_CY" 272
chk "touchpad: a press turns it off, the switch at once" until_is 2 0 ctl_part touchpad
chk "touchpad: ...in the compositor's device config" test "$(hq awesome pad)" = off
sleep 0.6 # past the policy's settle: nothing may put it back
chk "touchpad: ...and the click's pointer leaving keeps the choice" test "$(hq awesome pad)" = off
click "$(cc_btn_cx 1)" "$CC_CY" 272
chk "touchpad: the next press turns it back on" until_is 2 1 ctl_part touchpad
chk "touchpad: ...and no press posted a capsule under the panel" test "$(st)" = "center:0 live:0 dnd:0"
hq awesome controlcenter >/dev/null
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
