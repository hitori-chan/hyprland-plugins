# ---- hyprosd wpctl process path -------------------------------------------
# The nested compositor shadows only wpctl, so this reaches the real Lua,
# deferred queue, pidfd, pipe readback, and notification paths without changing
# the live PipeWire sink. The fake validates argv and emits real wpctl output.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/notify-lib.sh"
: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_up()"; sleep 0.4
chk "hyprosd: volume up uses the capped relative wpctl command" grep -Fxq "set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+" "$STATE/wpctl.log"
chk "hyprosd: volume up readback produces an OSD card" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

: > "$STATE/flood-wpctl"
dsp "hl.plugin.hyprosd.volume_up()"; sleep 0.5
chk "hyprosd: flood readback closes at the retained-output cap" test -s "$STATE/flood-wpctl.closed"
chk "hyprosd: flood readback emits no guessed feedback" test "$(st)" = "center:0 live:0 dnd:0"
chk "hyprosd: flood leaves the nested compositor responsive" hq_matches '^center:0 live:0 dnd:0$' hyprnotify state
rm -f "$STATE/flood-wpctl" "$STATE/flood-wpctl.closed"

: > "$STATE/wpctl.log"
dsp "hl.plugin.hyprosd.volume_down()"; sleep 0.4
chk "hyprosd: volume down uses the relative wpctl command" grep -Fxq "set-volume @DEFAULT_AUDIO_SINK@ 5%-" "$STATE/wpctl.log"
chk "hyprosd: volume down readback produces an OSD card" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

: > "$STATE/wpctl.log"
dsp "(function() for _ = 1, 32 do hl.plugin.hyprosd.volume_up() end return hl.dsp.no_op() end)()"; sleep 0.8
chk "hyprosd: repeat backpressure caps active chains" test "$(grep -c '^set-volume ' "$STATE/wpctl.log")" = 16
chk "hyprosd: repeat backpressure preserves admitted feedback" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9993 >/dev/null 2>&1; sleep 0.2

# ---- semantic OSD icons ----------------------------------------------------
# A fixed OSD id is replaced throughout a key sweep. Capture the actual icon
# cell after each semantic state; equal crops would mean a stale texture or a
# theme-resolution fallback has collapsed the controls back to one glyph.
osd_notify() {
	nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
		Notify susssasa\{sv\}i osd 9992 "$1" OSD "" 0 2 urgency y 0 x-hitori-osd b true 30000 >/dev/null 2>&1
}
osd_icon_hash() {
	local out="$STATE/osd-$1.png"
	capture_nested "$out" || return 1
	magick "$out" -crop "40x40+$((POP_CARD_X + 16))+$((34 + 16))" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1
}
osd_notify display-brightness-symbolic; sleep 0.5
BRIGHT_ICON="$(osd_icon_hash brightness)"
osd_notify audio-volume-high; sleep 0.5
VOLUME_ICON="$(osd_icon_hash volume)"
osd_notify touchpad-disabled; sleep 0.5
TOUCHPAD_ICON="$(osd_icon_hash touchpad)"
if [[ -n "$BRIGHT_ICON" && -n "$VOLUME_ICON" && -n "$TOUCHPAD_ICON" && "$BRIGHT_ICON" != "$VOLUME_ICON" && "$BRIGHT_ICON" != "$TOUCHPAD_ICON" && "$VOLUME_ICON" != "$TOUCHPAD_ICON" ]]; then
	ok "OSD icons: fixed-id brightness, volume, and touchpad pixels differ"
else
	bad "OSD icons: fixed-id brightness, volume, and touchpad pixels differ"
fi
chk "OSD icons: fixed-id replacements retain one live card" test "$(st)" = "center:0 live:1 dnd:0"
osd_notify audio-volume-high; sleep 0.3
hq hyprnotify clear >/dev/null; sleep 0.4
chk "Clear all: private OSD feedback is not dismissible" test "$(st)" = "center:0 live:1 dnd:0"
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications CloseNotification u 9992 >/dev/null 2>&1; sleep 0.3

# ---- OSD below an open shade ----------------------------------------------
# OSD-band cards are deliberately absent from shade rows and the bell badge,
# but their active surface must remain visible below a shade that was already
# open. Send the same fixed-id/value shape as hyprosd's brightness path, then
# click its popup hitbox. A working below-shade card dismisses while leaving
# the shade open; a hidden card leaves the OSD in the model or hits the panel.
hq hyprnotify center >/dev/null; sleep 0.5
nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications \
	Notify susssasa\{sv\}i osd 9992 display-brightness-symbolic Brightness 72% 0 3 value i 72 urgency y 0 x-hitori-osd b true 1200 >/dev/null 2>&1
sleep 0.35
chk "center OSD: brightness card is active" test "$(st)" = "center:1 live:1 dnd:0"
chk "center OSD: fixed card remains outside shade accounting" test "$(bd)" = "banners:0 resident:0"
# Measured with the shade open: the empty shade ends at y=175 (26+149), the
# band starts at 182, and the right-aligned OSD card spans 182..289, so its
# center is (PANEL_X + N_W/2 - 1, 235). The old formula assumed a
# closed-shade band and clicked the shade's own glass.
OSD_X=$((PANEL_X + N_W / 2 - 1))
OSD_Y=235
click "$OSD_X" "$OSD_Y" 272
chk "center OSD: below-shade popup hitbox dismisses the card" test "$(st)" = "center:1 live:0 dnd:0"
sleep 1.4
chk "center OSD: card expires without closing the shade" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.4

# ---- pointer-only shade ownership -------------------------------------------
# The center has no keyboard navigation or notification action map. Its close
# paths are pointer-visible: an outside click closes the shade without
# dismissing the card, while a right-click on an open manage panel closes only
# that panel and leaves the shade standing.
dsp "hl.dsp.exec_cmd('notify-send -t 60000 \"pointer center\" body')"; sleep 1
hq hyprnotify center >/dev/null; sleep 0.7
outside_click
chk "center pointer: outside click closes the shade" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify center >/dev/null; sleep 0.5
longpress "$ROWX" "$ROWY" 272
chk "center pointer: long-press opens management" test "$(st)" = "center:1 live:1 dnd:0"
click "$ENTX" "$MANAGE_DEFAULT_SELECTED_Y" 273
chk "center pointer: right-click closes management only" test "$(st)" = "center:1 live:1 dnd:0"
outside_click
chk "center pointer: outside click closes the remaining shade" test "$(st)" = "center:0 live:1 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.8

# ---- banners while fullscreen -----------------------------------------------
# A real fullscreen window must not hide ordinary banners by default: the
# renderer already exits scanout for visible cards, which keeps notifications
# available over games and video just like the OSD path. A checkerboard beneath
# the popup also makes the live-blur boundary measurable: blur must change an
# interior sample without changing pixels in the rounded-away corner.
BLUR_PATTERN="$STATE/fullscreen-blur-pattern.png"
BLUR_FRAME="$STATE/fullscreen-blur-on.png"
FLAT_FRAME="$STATE/fullscreen-blur-off.png"
magick -size "${MON_W}x${MON_H}" pattern:checkerboard "$BLUR_PATTERN"
dsp "hl.dsp.exec_cmd('mpv --no-config --loop-file=inf --really-quiet $BLUR_PATTERN')"; sleep 2
dsp "hl.dsp.window.fullscreen()"; sleep 1
# mode 2 is FSMODE_FULLSCREEN; 1 is merely maximized and must NOT count
expect "fullscreen: a window really is fullscreen" "any(c['fullscreen'] == 2 for c in cs)"
# The production default is an opaque surface container, which makes the fork
# skip blur work entirely. This one probe deliberately opts the popup surface
# into glass so it can keep guarding rounded blur clipping and live sampling.
hq eval 'hl.config({ plugin = { hyprnotify = { col_surface = 0x9e172025 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a q1 -t 30000 visible body')"; sleep 1.2
chk "fullscreen: ordinary banner remains visible by default" test "$(bd)" = "banners:1 resident:0"
capture_nested "$BLUR_FRAME"
hq eval 'hl.config({ decoration = { blur = { enabled = false } } })' >/dev/null; sleep 0.6
capture_nested "$FLAT_FRAME"
CARD_X=$N_EDGE
# The outside sample must sit OUTSIDE the popup surface AND its 16px shadow:
# the shadow is semi-transparent surface ink, so any crop within its reach
# legitimately shows the blur on/off difference and cannot gate clipping.
# v14 banners are full-width, so the 16px screen margin equals the shadow
# reach and there is no horizontal clear strip; the 24px-below-the-bottom
# sample (plain banner: y 26..132, shadow to 148) clears both. A blur
# composite that leaks past the surface bounds still trips it.
CORNER_DELTA="$(magick "$BLUR_FRAME" "$FLAT_FRAME" -compose difference -composite \
	-crop "8x8+$((CARD_X + 4))+156" +repage -colorspace sRGB -format '%[fx:mean]' info: 2>/dev/null)"
INTERIOR_DELTA="$(magick "$BLUR_FRAME" "$FLAT_FRAME" -compose difference -composite \
	-crop "12x12+$((CARD_X + 280))+$((34 + 44))" +repage -colorspace sRGB -format '%[fx:mean]' info: 2>/dev/null)"
chk "rounded blur: clipped corner matches the no-blur surface" awk '{ exit !($1 < 0.01) }' <<<"$CORNER_DELTA"
chk "rounded blur: interior still samples live blur" awk '{ exit !($1 > 0.01) }' <<<"$INTERIOR_DELTA"
hq eval 'hl.config({ decoration = { blur = { enabled = true } } })' >/dev/null; sleep 0.6
hq eval 'hl.config({ plugin = { hyprnotify = { col_surface = 0xff172025 } } })' >/dev/null; sleep 0.4

# quiet_fullscreen remains an explicit opt-in for users who want the old quiet
# behavior. Existing banners stay visible; only new ordinary arrivals park.
hq eval 'hl.config({ plugin = { hyprnotify = { quiet_fullscreen = 1 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.exec_cmd('notify-send -a q2 -t 30000 quiet body')"; sleep 1.2
chk "fullscreen: opt-in quiet mode parks ordinary cards" test "$(bd)" = "banners:1 resident:1"
dsp "hl.dsp.exec_cmd('notify-send -a q3 -u critical \"loud\" body')"; sleep 1.2
chk "fullscreen: critical still punches through quiet mode" test "$(bd)" = "banners:2 resident:1"
hq hyprnotify clear >/dev/null; sleep 0.5
hq eval 'hl.config({ plugin = { hyprnotify = { quiet_fullscreen = 0 } } })' >/dev/null; sleep 0.4
dsp "hl.dsp.window.fullscreen()"; sleep 1
dsp "hl.dsp.exec_cmd('notify-send -a q4 -t 30000 loudagain body')"; sleep 1.2
chk "fullscreen: after exit, banners remain available" test "$(bd)" = "banners:1 resident:0"
hq hyprnotify clear >/dev/null; dsp "hl.dsp.window.close()"; sleep 1

# ---- inline reply -----------------------------------------------------------
# The protocol the Linux chat apps speak: a sender only offers a reply when the
# server advertises the capability, so the capability IS the feature. Driven
# The visible Reply chip is pointer-armed first; only the resulting field owns
# the keyboard for text entry. Assert the reply signal on the wire — a card
# closing proves nothing on its own, since firing its primary closes it too.
chk "reply: the capability is advertised" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications GetCapabilities | grep -q inline-reply"
chk "reply: NotificationReplied is on the interface" \
	bash -c "nbus() { DBUS_SESSION_BUS_ADDRESS='$NBUS' busctl --user \"\$@\"; }; nbus introspect org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications | grep -q NotificationReplied"
REPLIED="$STATE/replied.log"
rm -f "$REPLIED"
( DBUS_SESSION_BUS_ADDRESS="$NBUS" timeout 120 busctl --user monitor --match "type='signal',member='NotificationReplied'" >"$REPLIED" 2>&1 & )
sleep 0.5
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
# A heads-up Reply never opens the shade in v13: it invokes the app and
# dismisses (spec §5), so the inline field is armed from the shade's expanded
# conversation kid — chip, kid chevron, then the kid's Reply button.
arm_reply() {
	[[ "$(st)" == center:1* ]] && { hq hyprnotify center >/dev/null; sleep 0.4; }
	hq hyprnotify clear >/dev/null; sleep 0.4 # keep the new card in slot 1
	conv_reply_notify Telegram "chat-arm-$1" "sender-arm" "$1" "mid-$1" "are you around?"
	sleep 1
	hq hyprnotify center >/dev/null; sleep 0.5
	click "$CHIPX" 74 272                     # open the card (one 55px kid)
	click "$KIDCHEV_X" "$KIDCHEV_Y" 272       # open the kid with its Reply
	click "$REPLY_BTN_X" "$REPLY_BTN_Y" 272   # arm the inline field
}
last_reply_is() {
	python3 - "$REPLIED" "$1" <<'PY'
import re, sys
data = open(sys.argv[1], encoding="utf-8").read()
values = re.findall(r'STRING "([^"\\]*)"', data)
raise SystemExit(0 if values and values[-1] == sys.argv[2] else 1)
PY
}
last_reply_bytes() {
	python3 - "$REPLIED" "$1" <<'PY'
import re, sys
data = open(sys.argv[1], encoding="utf-8").read()
values = re.findall(r'STRING "([^"\\]*)"', data)
raise SystemExit(0 if values and len(values[-1].encode()) == int(sys.argv[2]) else 1)
PY
}

arm_reply "Edit controls"
chk "reply: the shade's expanded kid Reply arms the inline field" test "$(st)" = "center:1 live:1 dnd:0"
reply_keys 'tap enter'
chk "reply: empty Enter keeps the field armed" test "$(st)" = "center:1 live:1 dnd:0"
reply_keys $'tap a\ntap b\ntap c\ntap d\ntap left\nmods shift\ntap left\nmods none\ntap home\ntap right\nmods shift\ntap right\nmods none\ntap x\ntap delete\ntap backspace\ntap home\nmods shift\ntap right\nmods none\ntap end\ntap z\ntap enter'
chk "reply: cursor, selection, replacement, delete, and boundaries submit correctly" last_reply_is adz
chk "reply: edited card is dismissed only after submit" test "$(st)" = "center:1 live:0 dnd:0"

# Reply stays reachable on a grouped conversation: two senders fold into one
# card and the NEWEST sender's expanded kid carries the Reply chip. The v13
# model merges a conversation into ONE entry, so arming keeps live:1 and
# submitting closes the whole card — the same contract as every other
# dismissal path on a merged conversation.
conversation_notify Telegram team-grp "Team" sibling Sibling grp-1 "lunch?" group
sleep 0.6
conv_reply_notify Telegram team-grp teammate Teammate grp-2 "Reply from the expanded child" group
sleep 0.8
expect_panel "reply/group: two senders fold into one two-line card" "$STATE/reply-group-card.png" 172
click "$CHIPX" 74 272
expect_panel "reply/group: the chip opens both sender kids" "$STATE/reply-group-open.png" 288
click "$KIDCHEV_X" "$KIDCHEV_Y" 272
expect_panel "reply/group: the newest kid opens with its Reply chip" "$STATE/reply-group-kid.png" 345
GROUP_REPLY_X=$((PANEL_X + 112)) # the kid Reply text center
GROUP_REPLY_Y=186                # same first-kid geometry as REPLY_BTN_Y
click "$GROUP_REPLY_X" "$GROUP_REPLY_Y" 272
expect_panel "reply/group: the armed field takes 345px" "$STATE/reply-group-armed.png" 357
chk "reply/group: expanded child Reply arms without dismissing its group" test "$(st)" = "center:1 live:1 dnd:0"
reply_keys $'tap g\ntap r\ntap o\ntap u\ntap p\ntap e\ntap d\ntap enter'
chk "reply/group: expanded child emits NotificationReplied" last_reply_is grouped
chk "reply/group: submit closes the merged conversation card" test "$(st)" = "center:1 live:0 dnd:0"
hq hyprnotify clear >/dev/null; sleep 0.6

arm_reply "UTF-8 paste"
clipboard_now 'a🙂b'
reply_keys $'mods ctrl\ntap v\nmods none\ntap left\ntap left\ntap delete\ntap enter'
chk "reply: Ctrl+V and UTF-8 cursor deletion preserve codepoint boundaries" last_reply_is ab
clipboard_stop

LONG_REPLY="$(python3 -c 'import sys; sys.stdout.write("BEGIN-" + "x" * 1990 + "-END" + "ignored")')"
arm_reply "Long paste"
clipboard_now "$LONG_REPLY"
reply_keys $'mods ctrl\ntap v\nmods none'
REPLY_END_FRAME="$STATE/reply-scroll-end.png"
REPLY_HOME_FRAME="$STATE/reply-scroll-home.png"
capture_nested "$REPLY_END_FRAME"
reply_keys 'tap home'
capture_nested "$REPLY_HOME_FRAME"
REPLY_FIELD_X=$((PANEL_X + 82))
REPLY_FIELD_Y=167 # measured: the armed field's text line inside the expanded kid
REPLY_FIELD_END_HASH="$(magick "$REPLY_END_FRAME" -crop "140x18+$((REPLY_FIELD_X + 8))+$((REPLY_FIELD_Y - 9))" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1)"
REPLY_FIELD_HOME_HASH="$(magick "$REPLY_HOME_FRAME" -crop "140x18+$((REPLY_FIELD_X + 8))+$((REPLY_FIELD_Y - 9))" +repage -depth 8 rgba:- 2>/dev/null | sha256sum | cut -d' ' -f1)"
REPLY_OUTSIDE_DELTA="$(magick "$REPLY_END_FRAME" "$REPLY_HOME_FRAME" -compose difference -composite \
	-crop "44x18+$((REPLY_FIELD_X - 50))+$((REPLY_FIELD_Y - 9))" +repage -colorspace sRGB -format '%[fx:mean]' info: 2>/dev/null)"
# v13 renders the draft as ONE elided line (cachedText clips with an
# ellipsis); there is no horizontal view to scroll, so the field must look
# identical with the cursor parked at either end and paint nothing outside.
chk "reply: a long draft stays one elided line at either cursor end" test "$REPLY_FIELD_END_HASH" = "$REPLY_FIELD_HOME_HASH"
chk "reply: the field paints nothing outside the editor" awk '{ exit !($1 < 0.01) }' <<<"$REPLY_OUTSIDE_DELTA"
reply_keys $'tap end\ntap enter'
chk "reply: pasted drafts are bounded to 2 KiB" last_reply_bytes 2000
clipboard_stop

arm_reply "Cancelled paste"
CLIP_CANCEL_LOG="$STATE/clip-cancel.log"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 700 stale >"$CLIP_CANCEL_LOG" 2>&1 & CLIP_PID=$!
for _ in $(seq 1 30); do grep -qx READY "$CLIP_CANCEL_LOG" 2>/dev/null && break; sleep 0.1; done
chk "reply: delayed clipboard source owns the nested selection" grep -qx READY "$CLIP_CANCEL_LOG"
reply_keys $'mods ctrl\ntap v\nmods none\ntap esc'
wait "$CLIP_PID" 2>/dev/null || true; CLIP_PID=""; sleep 0.2
# Re-arm through the v13 shade path: the late callback must not mutate the
# fresh field. ESC closes only the field — the card stays open with its kid
# expanded, so the Reply button is still at REPLY_BTN_Y; re-clicking the chip
# here would FOLD the open card instead.
click "$REPLY_BTN_X" "$REPLY_BTN_Y" 272
reply_keys $'tap 24\ntap 37\ntap enter'
chk "reply: closing the editor invalidates a late clipboard callback" last_reply_is ok

arm_reply "Timed paste"
CLIP_TIMEOUT_LOG="$STATE/clip-timeout.log"
WAYLAND_DISPLAY="$WL" "$REPO/devtools/cliphold" 2500 stale >"$CLIP_TIMEOUT_LOG" 2>&1 & CLIP_PID=$!
for _ in $(seq 1 30); do grep -qx READY "$CLIP_TIMEOUT_LOG" 2>/dev/null && break; sleep 0.1; done
reply_keys $'mods ctrl\ntap v\nmods none'
chk "reply: delayed source received the asynchronous paste request" grep -qx SEND "$CLIP_TIMEOUT_LOG"
sleep 1.7
reply_keys $'tap 24\ntap 37'
wait "$CLIP_PID" 2>/dev/null || true; CLIP_PID=""; sleep 0.2
reply_keys 'tap enter'
chk "reply: clipboard timeout cannot mutate the later draft" last_reply_is ok
hq hyprnotify center >/dev/null; sleep 0.4
hq hyprnotify clear >/dev/null; sleep 0.8

# The launcher shares the bounded asynchronous clipboard helper but owns a
# larger 4 KiB query budget. Execute a comment-only tail so the isolated
# history file records the exact admitted UTF-8 query without side effects.
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
