# awesome/gate/quick.sh — the dev-loop smoke battery (~30 s): the load,
# the strip, one window lifecycle, the CSD content-frame box (the 2026-10-04
# bug domain), one maximize round-trip and one notification. It is the
# gate's default run; `-b all` adds the full module batteries. Every wait is
# on state (lib.sh's bounded polls), none on the clock.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---- the strip -------------------------------------------------------------
# The strip warms in over its first frames after launch (the warm/draw gate
# never paints in the frame that creates a texture); wait for it to draw,
# bounded — a strip that never draws still fails the check.
chk "quick: the strip is on screen (clock, bell and viewed tag draw)" \
	shot_until 5 "$STATE/quick-strip.png" strip_drawn "$STATE/quick-strip.png"

# ---- one window lifecycle ---------------------------------------------------
# the title draws IN the strip's task band (lib.sh task_band): baseline
# from the strip capture above, then the window's title must add lit pixels
CHIP0="$(task_band "$STATE/quick-strip.png")"
chip_lit() { (($(task_band "$1") > CHIP0 + 100)); }
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=600x300')"; settled foot
expect "quick: the spawn keeps its requested 600x300 at the remembered (100,100)" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[600,300] for c in cs)"
chk "quick: the task chip draws with the window" shot_until 3 "$STATE/quick-chip.png" chip_lit "$STATE/quick-chip.png"
# the float rule (stress cfg) makes foot floating; maximize/unmaximize must
# round-trip its floating box exactly. Poll until the maximize actually
# landed before unmaximizing: toggling before the first toggle registered
# sent a second MAXIMIZE (a no-op), leaving the window floating at workarea
# size (2026-10-04 quick battery)
REFBOX="$(box4 foot)"
dsp "hl.plugin.awesome.maximize()"
expect_by 3 "quick: maximize covers the workarea" \
	"any(c['class']=='foot' and c['size'][0]>$((MON_W-8)) and c['size'][1]>$((MON_H-80)) for c in cs)"
dsp "hl.plugin.awesome.maximize()"
chk "quick: unmaximize restores the remembered floating box" until_is 4 "$REFBOX" box4 foot ||
	echo "  diag: refbox='$REFBOX' now='$(box4 foot)'" >&2
# spot memory: close + respawn round-trips the position; a different
# requested size is the client's to keep
close_wait "$(class_addr foot)"
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x400')"; settled foot
expect "quick: the spot round-trips after close+respawn, at the requested 500x400" \
	"any(c['class']=='foot' and c['at']==[100,100] and c['size']==[500,400] for c in cs)"

# ---- CSD content frame (the 2026-10-04 bug) --------------------------------
dsp "hl.dsp.exec_cmd('$REPO/devtools/splashwin 300 350 10 quickcsd')"; settled quickcsd
expect "quick: pinned CSD box is the content frame (300x350), centered" \
	"any(c['class']=='quickcsd' and c['floating'] and c['size']==[300,350] and abs(c['at'][0]-$(( (MON_W-300)/2 )))<=14 and abs(c['at'][1]-$(( 30+(MON_H-30-350)/2 )))<=14 for c in cs)"
capture_nested "$STATE/quick-csd.png"
chk "quick: CSD content fills the box, the margin is cropped" csd_cropped "$STATE/quick-csd.png" "$(box4 quickcsd)"

# ---- one notification -------------------------------------------------------
# polled, bounded: a fresh nested's private dbus session settles in; one
# read racing the settle failed for the wrong reason (2026-10-04)
dsp "hl.dsp.exec_cmd('notify-send -a quick smoke body')"
chk "quick: the notification pops as a banner" until_is 6 "banners:1 resident:0" hq awesome badge ||
	echo "  diag: badge='$(hq awesome badge)' state='$(st)' count='$(hq awesome count)'" >&2
# while the banner is up the card sits in the banners bucket (it joins the
# live resident set only on retreat), so the model proof is the count
chk "quick: the card is kept in the model" test "$(hq awesome count)" = 1
hq awesome clear >/dev/null
chk "quick: clear sweeps the card" until_is 3 0 hq awesome count

# ---- trailer ----------------------------------------------------------------
close_wait "$(class_addr foot)"
close_wait "$(class_addr quickcsd)"
chk "quick: the desktop is left window-free" test "$(pyc "sum(1 for c in cs)")" = 0
