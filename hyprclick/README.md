# hyprclick

Awesome's click and focus-raise policy. No config.

1. **Click-to-raise** — a plain left click raises the clicked window;
   clicking a maximized window tucks the floaters back behind it. Skips
   presses another plugin swallowed (load `hyprmax` first).
2. **Keyboard focus raises, hover never does** — binds and dispatchers
   raise; sloppy focus doesn't.
3. **`hl.plugin.hyprclick.focus_prev_here()`** — the previously focused
   window on the current workspace; repeated presses bounce between the
   two most recent windows (awesome's Mod+Tab).
4. **`hl.plugin.hyprclick.focus_next()` / `focus_prev()`** — cycle the
   workspace's windows in arrival order, wrapping (awesome's Mod+J/K,
   `focus.byidx`). The native cycle walks the z-order list, which rule
   2's raises reshuffle — backward cycling bounced instead of walking.

Maximize itself lives in `hyprmax`.
