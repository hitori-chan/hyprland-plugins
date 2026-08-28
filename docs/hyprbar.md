# hyprbar contracts

User behavior and configuration live in
[`hyprbar/README.md`](../hyprbar/README.md). This file keeps only cross-file
maintenance contracts.

## Rendering And Input

The bar, launcher, and tray menus are compositor-drawn. Before intercepting
input they recheck session lock, native layers/popups/IME surfaces, seat and
implicit pointer grabs, and input-capture ownership. Native ownership wins and
clears partial plugin input state.

Geometry and hitboxes remain monitor-local. Fixed widget slots and tray clipping
protect the task area on narrow outputs. Textures use `common/texcache.hpp`:
warm on the event loop, draw no earlier than the next frame, scissor to damage,
and damage both old and new geometry for visible transitions.

## Tasks

Hyprland has no plugin-owned minimized state. `hyprbar` removes a minimized
window from layout/render participation and remembers its fullscreen, maximize,
and floating state for restoration. Tasks stay in arrival order and focus
cycling skips minimized windows.

Client minimize, task clicks, and plugin actions share one path. Activation can
restore a minimized client only when `misc:focus_on_activate` permits it.
Workspace and focus changes are deferred through `NHyprCommon::CHop`; teardown
invalidates pending hops.

## Tray And Bell

The tray owns its StatusNotifier/dbusmenu connection and the bus used by battery
alerts and the bell. Calls originating in render/input are posted through the
bounded shared queue. Teardown removes event sources and borrowed proxies before
destroying the connection.

The bell talks to `hyprnotify` only through `org.hitori.hyprnotify` on
`/org/freedesktop/Notifications`. It consumes `State` and posts `Toggle`; no
plugin symbols or duplicate center state are shared.

Icon resolution follows the active GTK theme, hicolor, and pixmaps. Menus support
updates, cascades, scrolling, and check/radio state. Tray keyboard navigation,
tooltips, and overlay icons are not implemented.

## Launcher

Desktop discovery and completion run in a bounded cancellable helper process.
The compositor drains framed results through its event loop; queue backpressure
pauses the helper, and teardown closes the pipe and terminates its private
process group without joining filesystem work. Desktop Entry strings are decoded
before use; `Exec=` preserves its quoting and field-code grammar before passing
argument boundaries to Hyprland's executor. History is stored atomically under
`$XDG_CACHE_HOME/hyprbar/` (or `~/.cache/hyprbar/`).

| Keys | Action |
|---|---|
| `Left/Right`, `C-j/k`, `Home/End` | select |
| `Enter` | run selected entry |
| `C-Return`, `C-M-Return` | run raw query directly or in a terminal |
| `Tab`, `Shift-Tab` | complete command/path |
| `Up/Down`, `C-p/n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | edit |
| `C-v` | paste asynchronously |
| `Escape` or pointer click | close |

Typed and pasted input shares a 4 KiB UTF-8 limit. Clipboard transfer is
nonblocking with a 1.5-second deadline; closing or tearing down the launcher
removes the fd and invalidates the callback.

The launcher can request composition above fullscreen but cannot appear over a
direct-scanout client plane. Reload invalidates font/color-dependent and
disk-resolved icon textures.
