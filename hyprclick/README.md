# hyprclick

Click/focus policy with no configuration.

- A plain left click raises the target. Clicking a maximized window tucks
  fullscreen-flagged floaters without rewriting z-order.
- Keyboard focus raises; pointer hover does not.
- A short-lived corpse guard consumes presses aimed at a window that just
  closed or left fullscreen, preventing click-through to the window below.
- `hl.plugin.hyprclick.focus_prev_here()` toggles the two most recent windows
  on the current workspace.
- `focus_next()` and `focus_prev()` cycle stable arrival order instead of the
  z-order that click-to-raise continuously changes.

`hyprmax` owns maximize and must load first so its drag swallow wins.
