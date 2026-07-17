# hyprmax

Awesome's per-window maximize. No config.

- **Per-window flag, any number at once** — never enters compositor
  fullscreen state: the client is told (xdg `set_maximized`) and sized to
  the workarea. Compositor-maximized windows (initial-maximize, app
  requests) unmaximize through the native controller.
- **Immovable while maximized** — Super+click drags on a maximized window
  are swallowed. Loads before `hyprclick` so the swallow wins.
- **Windowed size remembered per app** — across closes and relogs
  (`$XDG_STATE_HOME/hyprmax/windowed.tsv`): un-maximizing a born-maximized
  window restores the app's last real windowed box.

## Lua

`hl.plugin.hyprmax.toggle()` — acts on the focused window.
