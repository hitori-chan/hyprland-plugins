# hyprbar contracts

User-visible behavior and configuration live in
[`hyprbar/README.md`](../hyprbar/README.md). This document records the
implementation contracts that affect compositor integration and maintenance.

## Rendering And Input

The bar, menubar, and tray menus are compositor-drawn regions rather than
Wayland surfaces. Before intercepting pointer or key input they recheck native
exclusive layers, popups, overlays, IME surfaces, top layers, seat grabs,
implicit pointer grabs, session lock, and active input-capture state. Native
ownership wins. Partial shell swallow state is cleared whenever that ownership
changes.

Bar geometry is monitor-local. Tag, task, tray, and layout hitboxes retain the
monitor that produced them, so an interaction never redirects to a same-numbered
workspace on another output. Fixed widget slots and tray clipping prevent
dynamic content from overlapping the tasklist.

Textures follow the shared warm/draw gate: creation happens only during a warm
pass, painting is scissored to damage, and every visible transition damages its
old and new geometry. Hover changes damage without rewarming.

## Task Lifecycle

Hyprland has no plugin-owned minimized flag. `hyprbar` removes a minimized
window from layout/render participation and keeps its previous fullscreen,
maximize, and floating state for restoration. Minimized tasks remain in arrival
order and focus cycling skips them.

Client minimize requests and the plugin actions use the same path. An
xdg-activation request restores a plugin-minimized window only when
`misc:focus_on_activate` permits focus stealing; otherwise urgency is retained
without exposing a hidden focused window. Deferred workspace/focus changes use
`NHyprCommon::CHop`, and teardown invalidates armed hops.

## Tray And Bell

The tray owns the session-bus connection used by StatusNotifier items,
dbusmenu proxies, notifications emitted by the battery widget, and the bell.
Bus construction from input/render callbacks is posted to the shared bounded
queue: at most 256 pending sends and 64 sends per idle drain. Teardown removes
event sources, resets borrowed proxies, and only then destroys the connection.

The bell communicates with `hyprnotify` only through
`org.hitori.hyprnotify` on `/org/freedesktop/Notifications`. It consumes one
initial `State` reply plus later `State` signals and posts `Toggle`; it does not
share plugin symbols or maintain a duplicate center-open state.

Tray icon resolution follows the GTK theme, hicolor, and pixmaps paths. Missing
art keeps an empty clickable cell instead of substituting a text glyph. Menus
support live updates, cascades, scrolling, check/radio state, and bounded
geometry; keyboard menu navigation and tooltips are not implemented.

## Menubar

Desktop discovery, window-class icon association, and completion enumeration
run in bounded cancellable worker batches. Opening the launcher never performs a
filesystem scan on compositor dispatch. Theme reload invalidates resolved icons
without restarting independent desktop indexes.

Desktop Entry values are decoded before visibility, category, and icon use.
`Exec=` keeps its own quoting grammar through field-code expansion and passes
the resulting argument boundaries to Hyprland's executor. Invalid entries are
skipped. Launch counts and history are atomically persisted under
`~/.cache/hyprbar/`.

| Keys | Action |
|---|---|
| `Left/Right`, `C-j/k` | select |
| `Home/End` | jump |
| `Enter` | run selected entry |
| `C-Return` | run raw query |
| `C-M-Return` | run raw query in terminal |
| `Tab` / `Shift-Tab` | command/file completion |
| `Up/Down`, `C-p/n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | edit |
| `Escape` or pointer click | close |

The launcher can draw over composited fullscreen but not direct scanout, where
the client plane bypasses compositor rendering.

## Reload And Limits

Configuration colors and fonts participate in texture cache keys. Reload also
re-probes icon directories and invalidates app, tray, menu, and layout textures.

Current limits: one registered layout (`floating`), no tray keyboard navigation
or overlay icons, and a text-rendered menubar cursor rather than an inverse-video
block.
