# awesome/gate/state.sh — the hostile state-file battery: a garbage/absurd
# unified state.tsv must load clamped, and the one-time legacy migration
# must filter row-by-row and consume its source. Battle-tested since the
# state redesign; split from windows so a geometry change does not pay
# the three relaunches (2026-10-04 gate trim).
# ---- hostile state file -------------------------------------------------
# The unified state file is the user-editable live file: a garbage line,
# an out-of-range number and an absurd spot must all be skipped or clamped,
# not fatal (the state.tsv-present check also makes the migration a no-op).
kill_nested
rm -f -- "$AW_STATE"
printf 'garbage\n42\nspot\t1e400\t0\t300\t200\tinffoot\nspot\t-100\t-100\t-50\t-50\tnegfoot\nspot\t100000\t100000\t400\t300\tfoot\n' > "$AW_STATE"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "hostile tsv: the monolith still loads" test "$(hq plugin list | grep -c Plugin)" = 1
dsp "hl.dsp.window.close()"; sleep 0.5
dsp "hl.dsp.exec_cmd('foot --window-size-pixels=500x300')"; sleep 2
expect "far-off-screen seed: stored size applied, clamped to ($((MON_W-401)),$((MON_H-301)))" \
	"any(c['class']=='foot' and c['at']==[$((MON_W-401)),$((MON_H-301))] and c['size']==[400,300] for c in cs)"
HF="$(clients | python3 -c "
import json,sys
print(next((c['address'] for c in json.load(sys.stdin) if c['class']=='foot'), ''))")"
[[ -n "$HF" ]] && dsp "hl.dsp.window.close({window=\"address:$HF\"})"; sleep 0.5
chk "hostile seed closed: no foot survives into the panel batteries" test "$(pyc "sum(1 for c in cs if c['class']=='foot')")" = 0

# ---- hostile LEGACY migration -------------------------------------------
# The one-time migration itself is hostile-file input: a garbage legacy with
# NO unified file must filter row-by-row and still land a usable store —
# and consume the source (the migration is one-time).
kill_nested
rm -f -- "$AW_STATE"
# the preflight migration CONSUMED (and rmdir'd) this dir; re-seeding must
# recreate it or the seed silently no-ops (2026-10-03: a failed redirect
# left the migration with no source and the focus batteries ran on empty
# spot memory)
mkdir -p "$(dirname "$LEG_SPOT")"
printf 'garbage\n1e400\t0\t300\t200\tinffoot\n-100\t-100\t-50\t-50\tnegfoot\n300\t200\t400\t300\tmigrfoot\n' > "$LEG_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "legacy migration: the hostile file filtered into state.tsv" \
	bash -c "! grep -q inffoot \"$AW_STATE\" && ! grep -q negfoot \"$AW_STATE\" && grep -q $'\tmigrfoot\$' \"$AW_STATE\""
chk "legacy migration: the consumed source is gone" test "! -e $LEG_SPOT"
# the live unified file wins once present: a re-seeded legacy must not clobber it
kill_nested
mkdir -p "$(dirname "$LEG_SPOT")"
printf '999\t999\t100\t100\tfoot\n' > "$LEG_SPOT"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }
chk "legacy migration: a second pass does not clobber the live store" \
	bash -c "! grep -q $'\tfoot\$' \"$AW_STATE\" || grep -q $'\tmigrfoot\$' \"$AW_STATE\""
kill_nested
rm -f -- "$AW_STATE"
launch_nested || { echo "relaunch FAILED"; exit 1; }
retarget || { echo "nested retarget FAILED after relaunch"; exit 1; }

