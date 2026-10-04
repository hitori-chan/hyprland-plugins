# awesome/gate/system.sh — the system module's behavior battery: the wpctl
# process path (volume/mic), the readback caps, the repeat backpressure,
# the logind brightness path (a fake logind + backlight), and the touchpad
# policy probe. Nothing reaches the host: the fake wpctl shadows the
# nested's PATH (the live PipeWire sink is never touched), and the
# brightness path talks to the nested's private system bus.
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
chk "system: repeat backpressure caps active chains" test "$(grep -c '^set-volume ' "$STATE/wpctl.log")" = 16
chk "system: the capped chains still produced one card" test "$(st)" = "center:0 live:1 dnd:0"
hq awesome clear >/dev/null; sleep 0.4

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
