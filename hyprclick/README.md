# hyprclick

Awesome's click and focus-raise policy. No config.

1. **Click-to-raise** — a plain left click raises the clicked window;
   clicking a maximized window tucks the floaters back behind it. Skips
   presses another plugin swallowed (load `hyprmax` first).
2. **Keyboard focus raises, hover never does** — binds and dispatchers
   raise; sloppy focus doesn't.
3. **`hl.plugin.hyprclick.focus_prev_here()`** — the previously focused
   window on the current workspace.

Maximize itself lives in `hyprmax`.
