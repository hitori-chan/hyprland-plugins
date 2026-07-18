# hyprosd

The awesome volume/brightness OSD, native. The XF86 media keys call
`hl.plugin.hyprosd.*` and a notification card with the value bar answers —
ids 9992 (brightness) / 9993 (volume) / 9995 (microphone), replaced in
place like the old scripts pinned them. No config. Replaces
`scripts/osd.sh`.

1. **Brightness** — fork-free: current/max from `/sys/class/backlight`,
   ±5% linear steps (the shown percent IS current/max), floor 2 raw so
   the panel never goes black, written through logind
   `Session.SetBrightness` on the system bus (the session owner needs no
   root, no udev rule). The card waits for logind's ack. Fast repeats
   step from the value just asked for, not stale sysfs.
2. **Volume / mic** — through `wpctl` so PipeWire stays out of the
   process: the set spawns, its pidfd tells the event loop when it is
   done, then the get spawns with stdout on a pipe the event loop
   drains. Two short forks per keypress instead of the script's shell
   pipeline; render/input never wait on anything. Volume caps at 100%,
   mute shows `muted`, the mic card says `live`/`muted`.
3. **Cards** — urgency low, 1200 ms, `value` hint for the daemon's 4px
   bar, no icon (the daemon's `fallback_icon_dir` rolls each card its
   face), sent async on the plugin's own event-loop-integrated
   session-bus connection. Bus death turns the cards off; the keys keep
   working.

Functions: `volume_up`, `volume_down`, `mute`, `mic_mute`,
`brightness_up`, `brightness_down` — bind them nil-guarded:

```lua
hl.bind({ mods = "", key = "XF86AudioRaiseVolume",
          press = function() local p = hl.plugin.hyprosd; if p then p.volume_up() end end })
```
