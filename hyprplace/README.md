# hyprplace

Spawn placement for floating windows. No config.

A new window is placed by the first rule that lands on free space:

1. **Where the app's last window closed** — per class, persisted across
   relogs (`$XDG_STATE_HOME/hyprplace/lastspot.tsv`). Position only; the
   size is always the client's.
2. **The workarea center.**
3. **The middle of the largest free rectangle** that fits it — of the
   biggest hole when nothing does.

Maximized windows, fullscreen windows, and floats sized to the whole
workarea cover no free space (and a workarea-covering close leaves no
spot). When the chosen spot already holds a window — the workarea fully
covered — spawns cascade off it diagonally instead of stacking. Windows
that chose their own spot (X11, dialogs anchored to a parent) keep it
while it's free; X11 override-redirect surfaces are left alone; the
result is clamped on-screen.
