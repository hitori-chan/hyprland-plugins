# hyprbar details

## Taglist

Awesome's exact state matrix: the viewed tag gets the focus colors, urgent
tags the urgent colors, everything else the plain text color. Occupancy is
the little corner square — filled when the tag holds the focused window,
hollow when merely occupied. viewtoggle/toggle_tag have no analog: a window
sits on exactly one workspace.

The bar carries the monitor that owns each hitbox. A click or wheel cycle on
one output therefore changes that output's local tag and never redirects focus
to a same-numbered workspace on another output; `Mod+click` still moves the
focused window without following it.

## Tasklist

Arrival order, stable across raises. State markers prefix the title:

- `⌃` — pinned (Hyprland's pin is ontop + sticky; presented as awesome's
  ontop marker, which `Super+T` toggled)
- `+` — maximized
- `✈` — floating, suppressed while maximized (awesome's rule); the
  floating-only setup floats everything, so today it rides every
  unmaximized task and starts discriminating when other layouts land

The focused task is cyan text on the plain bar (`tasklist_bg_focus` =
`bg_normal`), urgent tasks get the urgent background. Right-click opens the
all-clients menu (`awful.menu.client_list`): icons + titles, click jumps to
a window on any workspace.

**Minimize** is awesome's `client.minimized`, which the compositor has no
flag for: the plugin hides the window (a tiled window is dropped from the
layout; any fullscreen/maximize mode is held and re-entered on restore) and
tracks it in a stack. Minimized tasks stay in the list, muted
(`fg_minimize`), and the focus wheel skips them. Click the focused task to
minimize it; click any other task — minimized included — to restore + focus
it. `Mod+N` minimizes the focused window, `Mod+Ctrl+N` restores the most
recently minimized (`hl.plugin.hyprbar.minimize()` / `.restore()`). A
client's own minimize request — a CSD titlebar button (xdg `set_minimized`)
or X11 `IconicState` — is honored as the same action. A minimized floating
window's vacated box is force-repainted so its last frame can't linger.

An **activation request restores it**. Clicking a notification, a browser's
"switch to tab", any xdg-activation: the compositor's `CWindow::activate()`
raises and focuses the window but cannot un-hide it, because hiding it was
this plugin's doing and the compositor has no minimize state to reverse.
Left alone the focus lands on an unrendered window and the check_focus guard
bounces straight off it, so the click appears to do nothing. The plugin
restores it from `window.urgent`, which `activate()` emits before its own
`misc:focus_on_activate` gate — and reads that same option, because with it
off the user has asked that activation never steal focus, and un-minimizing
a window is exactly that. With it off, the chip's urgent tint is the answer.

Icons resolve from the GTK icon theme + hicolor + pixmaps, PNG or SVG;
`*-symbolic` SVGs are repainted with the bar foreground.

## Tray

An in-compositor StatusNotifierWatcher/Host with a native dbusmenu
renderer.

- Left click activates (or opens the menu for menu-only items), middle
  click sends SecondaryActivate, right click always opens the menu (falls
  back to ContextMenu). Scroll is forwarded to the app.
- The Status property is honored: Passive items hide, NeedsAttention swaps
  to the attention icon set.
- Menus behave like the GTK ones they replaced: submenus cascade beside
  their parent row on hover (GTK's 225ms popup delay) or click; a panel
  taller than the screen scrolls (wheel, or the `▴`/`▾` strips); open menus
  refresh live from update signals; check and radio items draw their state
  in a leading column; `disposition` warning/alert rows take the urgent
  color.
- nm-applet note: in indicator mode it merges its two X11 menus into one
  and implements no left-click action — upstream design.
- The tray is width-safe: on a narrow output it drops cells that cannot fit
  inside the assigned slot rather than painting over the tasklist.
- D-Bus method construction from tray input and render-triggered menu cleanup
  is posted to the shared event-loop bus queue. The queue keeps at most 256
  pending sends, drains at most 64 per idle turn, and drops pending work before
  owned proxies or the connection are torn down; input never performs an
  inline bus drain.

## Bell

The notification bell sits between the tray and the battery and is the one
widget whose state lives in another plugin. It reads hyprnotify over the
bus and never through a shared symbol — `org.hitori.hyprnotify` on the
`/org/freedesktop/Notifications` object, the sanctioned cross-plugin
channel.

- The badge is `live + kept` (bannered popups plus resident shade cards)
  and hides at zero. It arrives two ways: a `State` signal hyprnotify emits
  on every model change, and one `State` call at init so a bar that starts
  after the daemon is not blank until the next notification. Both land in
  the same change-detected setter, so an unchanged count costs no repaint.
- Left click calls `Toggle`. hyprbar never tracks whether the shade is
  open — hyprnotify owns that. The link is the tray's own connection
  (`Bell::init()` runs after `Tray::init()`), so the bell costs no second bus.
  Toggle calls are posted through that link rather than serialised in the
  pointer/button callback.

## Native input precedence

The bar, menubar, and tray menus are compositor-drawn regions rather than
Wayland layer surfaces. Before claiming a click, scroll, or hover, the plugin
asks the target fork's native input manager for a fresh hit test of exclusive
layers, layer popups, overlay layers, IME popups, and top layers. A real native
surface wins that stack even when the last pointer motion was intercepted by
the bar; the helper applies Hyprland's exclusive-fullscreen filter to top
layers as well. Existing client implicit pointer grabs and native seat grabs
(`xdg_popup` and focus-grab surfaces) remain authoritative, including while no
button is held. An active `hyprland-input-capture-v1` session owns the physical
event stream, so the plugin passes button, axis, key, and warp events through
without cancelling them and clears only partial shell swallow state.

## Layoutbox

The active workspace's layout icon (rightmost), from
`~/.config/hypr/icons/<name>.png`. Per-workspace state, awesome's per-tag
model. Cycling — awesome's buttons and chords:

| input                  | action          |
| ---------------------- | --------------- |
| click / wheel up       | next layout     |
| right-click / wheel dn | previous layout |
| `Super+Space`          | next layout     |
| `Super+Shift+Space`    | previous layout |

The registry holds one layout (`floating`) until other layouts are
implemented; cycling is a visible no-op until then, and the bar only
carries the state — a future layout engine enforces it.

## The menubar (`Mod+P`)

A "Run: " prompt, the awesome categories (`Enter` drills in,
`BackSpace`/`Escape` on empty backs out) and the `.desktop` apps, filtered
as you type — name or command line, substring, prefix matches and
most-launched entries first — plus a trailing `Exec: <query>` entry that
runs whatever was typed. Opening the prompt never blocks on discovery: its
catalogue and window-class icon associations arrive in bounded worker batches,
and the prompt refilters as they become available.

| keys | action |
|---|---|
| `Left/Right`, `C-j/k` | select |
| `Home/End` | jump |
| `Enter` | run (`Terminal=true` entries open in `plugin:hyprbar:terminal`) |
| `C-Return` | run the raw query |
| `C-M-Return` | run the raw query in the terminal |
| `Tab`/`Shift-Tab` | shell completion (`$PATH` for the command word, filenames after) |
| `Up/Down`, `C-p/n` | prompt history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | readline editing |
| `Escape`, any click | close |

Entries show a theme icon when one resolves and an empty reserved icon cell
otherwise; the raw command row uses the theme's `system-run` icon. Launch
counts and history persist in `~/.cache/hyprbar/`
(`menu_count_file`, `history_menu`); each cache rewrite is atomic. Desktop
Entry string/list values decode their freedesktop escapes before names, icons,
categories, and desktop visibility are used. `Exec=` keeps its separate
quoting grammar until field-code expansion, then retains its argument
boundaries when handed to Hyprland's executor; malformed entries are skipped.
Field codes are classified once for validation and expansion. The menubar
supplies no files or URLs, so standalone file/URL fields remove their argument
while embedded fields retain the surrounding text; malformed placement and
unknown or ambiguous fields still reject the entry.
Completion enumeration is likewise worker-owned and cancellable, so the first
Tab may wait briefly for its first matching batch without stalling keyboard
dispatch.

This is the only launcher: fuzzel and the `Mod+R`/`Mod+X`/`Mod+S` prompts
were dropped by choice.

## Config reload

Colors and fonts are part of every texture's cache key, so a changed value
misses the cache and rebuilds on its own. Everything resolved from DISK at
init does not: `config.reloaded` re-probes the icon dirs (they come from the
GTK theme name, read once), drops every resolved app/tray/menu icon and the
layoutbox's, and repaints. Desktop discovery and WM-class icon association are
independent bounded session indexes, so a theme reload does not restart their
filesystem walk.

## Limitations

- The menubar's cursor is a `▏` bar, not awesome's inverse-video block —
  the text renderer takes plain text, no pango markup.
- The tray is mouse-only: no tooltips, overlay icons or menu keyboard
  navigation. An item with no usable pixmap or themed icon keeps an empty
  clickable cell; the bar does not substitute a character.
- Over a direct-scanout fullscreen game the menubar cannot draw: scanout
  (`render:direct_scanout`) hands the plane to the client and bypasses
  compositing. Composited fullscreen (video, browser) is fine.
- Backlog: a manual idle-inhibit "coffee" tray toggle.
