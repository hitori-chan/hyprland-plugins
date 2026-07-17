# hyprplace

Spawn placement for floating windows. No config.

A new window is placed by the first rule that lands on free space:

1. **Where the app's last window closed** — per class, persisted across
   relogs (`$XDG_STATE_HOME/hyprplace/lastspot.tsv`). Position only; the
   size is always the client's.
2. **The workarea center.**
3. **The middle of the largest free rectangle** that fits it — of the
   biggest hole when nothing does.

Maximized and fullscreen windows cover no free space. Windows that chose
their own spot (X11, dialogs anchored to a parent) keep it while it's
free; X11 override-redirect surfaces are left alone; the result is clamped
on-screen.
