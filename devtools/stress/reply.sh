#!/usr/bin/env bash
# The reply and shared-clipboard battery: the hyprosd wpctl process path,
# the shade's pointer-only close, the inline-reply protocol, and the launcher
# clipboard (hyprbar's menubar paste shares the bounded asynchronous
# clipboard helper). Helpers live in notify-lib.sh; this file is battery
# code only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"

# ---- hyprosd wpctl process path -------------------------------------------
# The nested compositor shadows only wpctl, so this reaches the real Lua,
# deferred queue, pidfd, pipe readback, and notification paths without
# changing the live PipeWire sink. The fake validates argv and emits real
# wpctl output. v6 has no OSD surface: the x-hyprnotify-osd hint is just an
# unknown hint, so the feedback card is a plain 1200ms card and the battery
# tears each one down by its fixed bus id (9993, volume) to keep the model clean.
: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_up()"; sleep 0.4
chk "hyprosd: volume up uses the capped relative wpctl command" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
chk "hyprosd: volume up readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
closeid 9993; sleep 0.3
chk "hyprosd: the feedback card tears down by id" test "$(st)" = "center:0 live:0 dnd:0"

: > "$STATE/flood-wpctl"
dsp "hl.plugin.hyprosd.volume_up()"; sleep 0.5
chk "hyprosd: flood readback closes at the retained-output cap" test -s "$STATE/flood-wpctl.closed"
chk "hyprosd: flood readback emits no guessed feedback" test "$(st)" = "center:0 live:0 dnd:0"
chk "hyprosd: flood leaves the nested compositor responsive" hq_matches '^center:0 live:0 dnd:0$' hyprnotify state
rm -f "$STATE/flood-wpctl" "$STATE/flood-wpctl.closed"

: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_down()"; sleep 0.4
chk "hyprosd: volume down uses the relative wpctl command" grep -Fxq "set-volume @DEFAULT_AUDIO_SINK@ 5%-" "$STATE/wpctl.log"
chk "hyprosd: volume down readback produces a card" test "$(st)" = "center:0 live:1 dnd:0"
closeid 9993; sleep 0.3

: > "$STATE/wpctl.log"
dsp "(function() for _ = 1, 32 do hl.plugin.hyprosd.volume_up() end return hl.dsp.no_op() end)()"; sleep 0.8
chk "hyprosd: repeat backpressure caps active chains" test "$(grep -c '^set-volume ' "$STATE/wpctl.log")" = 16
chk "hyprosd: repeat backpressure preserves admitted feedback" test "$(st)" = "center:0 live:1 dnd:0"
closeid 9993; sleep 0.3
chk "hyprosd: reset after the wpctl battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- pointer shade ownership -------------------------------------------------
# The shade's close paths that only the pointer owns: an outside click closes
# the shade WITHOUT dismissing the card (the card's own dismissal is the
# body click, right click, delete, or a fired primary — all tested in the
# other batteries).
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"pointer center\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
outside_click
chk "center pointer: outside click closes the shade" test "$(st)" = "center:0 live:1 dnd:0"
chk "center pointer: and dismissed nothing" test "$(bd)" = "banners:0 resident:1"
hq hyprnotify center >/dev/null; sleep 0.5
tap esc
chk "center pointer: esc also closes it" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8
chk "center pointer: reset after the battery" test "$(st)" = "center:0 live:0 dnd:0"

# ---- inline reply -------------------------------------------------------------
# The protocol the Linux chat apps speak: a sender only offers a reply when
# the server advertises the capability, so the capability IS the feature.
# Driven entirely from the keyboard (↓ selects the card, Tab arms its field,
# letters type, Enter sends) and asserted on the WIRE — a card closing
# proves nothing on its own, since firing its primary closes it too.
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 60 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED" 2>&1 & )
sleep 0.5
conv_reply_notify Telegram "Alice" "are you around?"
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

# ---- the launcher's shared clipboard --------------------------------------------
# The launcher shares the bounded asynchronous clipboard helper but owns a
# larger 4 KiB query budget. Execute a comment-only tail so the isolated
# history file records the exact admitted UTF-8 query without side effects.
reply_keys() { printf '%s\n' "$1" | vk; sleep 0.65; }
clipboard_now() {
	clipboard_stop
	local log="$STATE/clip-now.log"
	WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 0 "$1" >"$log" 2>&1 & CLIP_PID=$!
	for _ in $(seq 1 30); do grep -qx READY "$log" 2>/dev/null && return; sleep 0.1; done
	echo "instant clipboard source did not reach READY" >&2
	clipboard_stop
	exit 1
}
clipboard_stop() {
	if [[ -n "$CLIP_PID" ]]; then
		kill "$CLIP_PID" 2>/dev/null || true
		wait "$CLIP_PID" 2>/dev/null || true
		CLIP_PID=""
	fi
}
QUERY_PAYLOAD="$(python3 -c 'import sys; sys.stdout.write("true # " + "a" * 4085 + "🙂" + "ignored")')"
clipboard_now "$QUERY_PAYLOAD"
dsp "hl.plugin.hyprbar.menubar()"; sleep 1
reply_keys $'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none'
LAUNCH_HISTORY="$STATE/cache/hyprbar/history_menu"
sleep 0.8
chk "launcher: Ctrl+V admits one valid UTF-8 query up to 4 KiB" python3 - "$LAUNCH_HISTORY" <<'PY'
import sys
lines = open(sys.argv[1], "rb").read().splitlines()
ok = bool(lines) and len(lines[-1]) == 4096 and lines[-1].decode().startswith("true # ") and lines[-1].endswith("🙂".encode())
raise SystemExit(0 if ok else 1)
PY
clipboard_stop
CLIP_LAUNCH_LOG="$STATE/clip-launch-cancel.log"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 700 stale >"$CLIP_LAUNCH_LOG" 2>&1 & CLIP_PID=$!
for _ in $(seq 1 30); do grep -qx READY "$CLIP_LAUNCH_LOG" 2>/dev/null && break; sleep 0.1; done
dsp "hl.plugin.hyprbar.menubar()"; sleep 0.5
reply_keys $'mods ctrl\ntap v\nmods none\ntap esc'
wait "$CLIP_PID" 2>/dev/null || true; CLIP_PID=""; sleep 0.2
clipboard_now true
dsp "hl.plugin.hyprbar.menubar()"; sleep 0.5
reply_keys $'mods ctrl\ntap v\nmods none\nmods ctrl\ntap enter\nmods none'
sleep 0.8
chk "launcher: closing the prompt invalidates a late paste" bash -c "test \"\$(tail -n 1 '$LAUNCH_HISTORY')\" = true"
clipboard_stop
