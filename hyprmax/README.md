# hyprmax

Per-window maximize without consuming Hyprland's fullscreen slot. No config.

- `hl.plugin.hyprmax.toggle()` maximizes the focused window to its current
  workarea and sends the native xdg maximized state.
- App- or compositor-maximized windows are adopted into the same model.
- Maximized windows follow workspace, output, and reserved-area changes.
- `Mod+click` drags are swallowed while maximized; `hyprmax` therefore loads
  before `hyprclick`.
- Windowed geometry is restored from
  `$XDG_STATE_HOME/hyprmax/windowed.tsv`, constrained by the current workarea
  and client size hints.
