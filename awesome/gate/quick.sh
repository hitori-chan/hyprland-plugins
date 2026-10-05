# awesome/gate/quick.sh — the dev-loop smoke battery (~1 min): the load,
# the strip, one window lifecycle, the CSD content-frame box (the 2026-10-04
# bug domain), one maximize round-trip and one notification. It is the
# gate's default run; `-b all`/`-b everything` add the full module batteries.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- the strip -------------------------------------------------------------
# The strip warms in over its first frames after launch (the warm/draw gate
# never paints in the frame that creates a texture); wait for it to draw,
# bounded — a strip that never draws still fails the check below.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if capture_nested "$STATE/quick-strip.png" && [[ "$(bar_bright "$STATE/quick-strip.png")" -gt 300 ]]; then
		break
	fi
	sleep 0.5
done
echo "quick: strip bright=$(bar_bright "$STATE/quick-strip.png")" >&2
chk "quick: the strip is on screen (clock + tags draw)" \
	test "$(bar_bright "$STATE/quick-strip.png")" -gt 250

# ---- one window lifecycle ---------------------------------------------------
# the chip draws IN the strip's task band (x 210-600 of the 30px strip):
# baseline from the strip capture above, then the chip must add bright there
CHIP0="$(strip_band "$STATE/quick-strip.png" 210 600 100)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; sleep 2
expect "quick: the spawn keeps its requested 600x300 at the remembered (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[600,300] for c in cs)"
capture_nested "$STATE/quick-chip.png"
CHIP1="$(strip_band "$STATE/quick-chip.png" 210 600 100)"
chk "quick: the task chip draws with the window" \
	test "$CHIP1" -gt $(( CHIP0 + 100 ))
# the float rule (stress cfg) makes foot floating; maximize/unmaximize must
# round-trip its floating box exactly. pyc is a 1/0 predicate — value
# extraction goes through clients directly.
REFBOX="$(clients | python3 -c "
import json,sys
f = next((c for c in json.load(sys.stdin) if c['class']=='foot'), None)
print(tuple(f['at']) + tuple(f['size']) if f else '')")"
dsp "hl.plugin.awesome.maximize()"
# poll until the maximize actually landed (presentation), then unmaximize:
# toggling before the first toggle registered sent a second MAXIMIZE (a
# no-op), leaving the window floating at workarea size (2026-10-04 quick
# battery — the state-churn battery's seconds-long gap between toggles is
# what hid this)
MAXED=0
for _ in $(seq 1 12); do
	MAXED="$(pyc "any(c['class']=='foot' and c['size'][0]>$((MON_W-8)) and c['size'][1]>$((MON_H-80)) for c in cs)")"
	[[ "$MAXED" == 1 ]] && break
	sleep 0.25
done
chk "quick: maximize covers the workarea" test "$MAXED" = 1
dsp "hl.plugin.awesome.maximize()"
RESTORED=0
for _ in $(seq 1 16); do
	RESTORED="$(clients | python3 -c "
import json,sys
f = next((c for c in json.load(sys.stdin) if c['class']=='foot'), None)
print(1 if f and tuple(f['at']) + tuple(f['size']) == $REFBOX else 0)")"
	[[ "$RESTORED" == 1 ]] && break
	sleep 0.25
done
if [[ "$RESTORED" != 1 ]]; then
	bad "quick: unmaximize restores the remembered floating box"
	echo "  diag: refbox='$REFBOX' clients=$(clients 2>/dev/null | python3 -c "import json,sys;print([(c['class'],c['at'],c['size'],c['floating']) for c in json.load(sys.stdin)])" 2>/dev/null)" >&2
else
	ok "quick: unmaximize restores the remembered floating box"
fi
# spot memory: close + respawn round-trips the position; a different
# requested size is the client's to keep
FF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$FF" ]] && dsp "hl.dsp.window.close({window=\"address:$FF\"})"; sleep 1
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x400')"; sleep 2
expect "quick: the spot round-trips after close+respawn, at the requested 500x400" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"

# ---- CSD content frame (the 2026-10-04 bug) --------------------------------
# box == the client's content frame, centered: the buffer's shadow margin
# is cropped at the box edge, never padding inside it.
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 quickcsd')"; sleep 2
expect "quick: pinned CSD box is the content frame (300x350), centered" \
	"any(c['class']=='quickcsd' and c['floating'] and c['size']==[300,350] and abs(c['at'][0]-$(( (MON_W-300)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-350)/2 )))<=14 for c in cs)"
CS="$(clients | python3 -c "
import json,sys
c = next((c for c in json.load(sys.stdin) if c['class']=='quickcsd'), None)
print(f\"{c['at'][0]} {c['at'][1]} {c['size'][0]} {c['size'][1]}\" if c else 'none')")"
capture_nested "$STATE/quick-csd.png"
chk "quick: CSD content fills the box, the margin is cropped" \
	test "$(python3 - "$STATE/quick-csd.png" "$CS" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load()
x, y, w, h = map(int, sys.argv[2].split())
MAROON = (48, 32, 32)
content = lambda dx, dy: px[x + dx, y + dy] == MAROON
ok = content(3, 3) and content(w - 4, 3) and content(3, h - 4) and content(w - 4, h - 4) \
   and content(w // 2, h // 2)
outside = lambda dx, dy: px[dx, dy] != MAROON
ok = ok and outside(x - 5, y + h // 2) and outside(x + w + 5, y + h // 2) \
      and outside(x + w // 2, y - 5) and outside(x + w // 2, y + h + 5)
print(1 if ok else 0)
PY
)" = 1

# ---- one notification -------------------------------------------------------
# poll, bounded: a fresh nested's private dbus session settles in; one read
# racing the settle fails for the wrong reason (2026-10-04 quick battery).
dsp "hl.dsp.exec_cmd('notify-send -a quick smoke body')"
BADGE=""
for _ in $(seq 1 12); do
	BADGE="$(hq awesome badge)"
	[[ "$BADGE" == "banners:1 resident:0" ]] && break
	sleep 0.5
done
[[ "$BADGE" == "banners:1 resident:0" ]] \
	&& ok "quick: the notification pops as a banner" \
	|| { bad "quick: the notification pops as a banner"; echo "  diag: badge='$BADGE' state='$(hq awesome state)' count='$(hq awesome count)'"; }
# while the banner is up the card sits in the banners bucket (it joins the
# live resident set only on retreat), so the model proof is the count
chk "quick: the card is kept in the model" test "$(hq awesome count)" = 1
hq awesome clear >/dev/null; sleep 0.6
chk "quick: clear sweeps the card" test "$(hq awesome count)" = 0

# ---- trailer ----------------------------------------------------------------
for c in foot quickcsd; do
	A="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='$c'), ''))")"
	[[ -n "$A" ]] && dsp "hl.dsp.window.close({window=\"address:$A\"})"
done
sleep 0.5
chk "quick: the desktop is left window-free" \
	test "$(pyc "sum(1 for c in cs)")" = 0
