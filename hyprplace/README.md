# hyprplace

Spawn placement for floating windows. No config.

A new window is placed by the first rule that lands on free space:

1. **Where the app's last window closed** — per class, persisted across
   relogs (`$XDG_STATE_HOME/hyprplace/lastspot.tsv`).
2. **The workarea center.**
3. **The middle of the largest free rectangle** that fits it — of the
   biggest hole when nothing does.

## Size

A genuinely **resizable** app (mpv, terminals, browsers) also reopens at
its last size, reimposed at spawn: the compositor owns the configure, so a
content-sizer like mpv can't drift back to its video size and an app that
self-remembers its size (Firefox) can't fight it — modelled on KWin's
"Remember". A **fixed-size dialog** (`min == max`) keeps the client's size
and is never resized, so nothing blinks. Resizability is read from the
xdg-toplevel min/max hints; X11 windows manage their own geometry. The
size lands in `lastspot.tsv` alongside the position (rows written before
1.2.0 hold position only, and warm up to full geometry after the app
closes once).

Maximized windows, fullscreen windows, and floats sized to the whole
workarea cover no free space (and a workarea-covering close leaves no
spot). When the chosen spot already holds a window — the workarea fully
covered — spawns cascade off it diagonally instead of stacking. Windows
that chose their own spot (X11, dialogs anchored to a parent) keep it
while it's free; X11 override-redirect surfaces are left alone; the
result is clamped on-screen.
