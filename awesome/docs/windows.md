# Windows

The `windows` module: one state machine per window (normal, minimized,
maximized), the click and focus policy, and all placement.

## Minimize

Hyprland has no minimized state: the plugin takes the window out of
layout and render and keeps its fullscreen, maximize and floating state
for the restore. Focus cycling skips minimized windows. Minimizing the
focused window focuses the workspace's most recently focused task
(awesome's autofocus), unraised.

`hyprctl awesome unminimize <address>` (Lua: `unminimize(address)`)
shows one window again without moving the user: it returns where it is,
unraised and unfocused, and focus and workspace stay put. A fullscreen
state held at minimize returns only on a workspace no monitor shows.
`hyprctl awesome minimized` (Lua: `minimized()`) lists what the plugin
hides. The Lua restore runs on the next loop turn, since a bind may call
it inside the keyboard emission.

## Maximize

Per window. A floating window fills the work area and carries the xdg
maximized state, leaving the fullscreen slot alone; a tiled window takes
the compositor's own maximize. For floating windows:

- A window the app or compositor maximized is adopted. The client's own
  unmaximize (CSD button, titlebar double-click) acts as `Mod+M` does.
- It follows workspace, output and reserved-area changes, and the
  pointer follows its geometry, so a window growing under a still cursor
  takes the next click.
- It is a [tile](#snap) and does not move (awesome's
  `mouse.client.move`): a `Mod`+drag is swallowed, a client-started
  titlebar drag ends at once.
- The restore uses the remembered windowed box, fitted to the current
  work area and the client's size hints.
- A grouped window is never minimized or maximized by the plugin.
- A plugin reload keeps both states: minimized windows are shown as the
  plugin stops; the next instance adopts windows still told maximized.

## Click and focus

- A left click raises its target. Clicking a fullscreen window tucks the
  floaters allowed over it behind it without rewriting the z-order.
- Keyboard focus raises; hover does not.
- A short corpse guard eats presses aimed at a window that just closed
  or left fullscreen, so they don't land on the window below.
- `focus_next`/`focus_prev` cycle in arrival order (stable under
  click-to-raise); `focus_prev_here` toggles the two most recent windows
  on this workspace. Both, and snap, work on an open special workspace.

### Activation: attention, not focus

awesome's `permissions.activate`: a background app asking to be
presented gets attention, never the keyboard.

| Event | Result |
|---|---|
| a new window maps | it takes focus (awesome's `awful.client.focus.filter`) |
| an xdg-activation ask, `misc:focus_on_activate` off (the fork's default) | its task turns urgent; a task, tray or card click or `Mod+U` completes it |
| the focused, visible window asks | nothing |
| a window on another workspace, or minimized, asks | urgency only |
| a taskbar or switcher activates (wlr-foreign-toplevel) | focused wherever it is, restored first if minimized |
| an X11 `_NET_ACTIVE_WINDOW` | urgency only: X11 can't authenticate it, and Wine sends it on every internal focus change |

- Focus clears urgency.
- `Mod+U` is `hl.dsp.focus({ urgent_or_last = true })`: the urgent
  window wins and the view follows it (awesome's
  `awful.client.urgent.jumpto`); a minimized one is restored, as is any
  window a focus dispatch names.
- `focus_on_activate = true` (global or a rule) is vanilla awesome: an
  ask on a visible window focuses it, a Wayland ask on a minimized one
  restores and focuses it. Another workspace stays urgency only, as does
  any window under a `suppress_event activate` (or `activatefocus`) rule.
- A taskbar's minimize and unminimize use the same state machine as the
  client's own button.
- Tray and card clicks focus the app's window from the compositor side,
  restoring it if its only window is minimized.

Urgency on activation asks and the X11 rule are fork extensions; upstream
ignores such asks.

## Spawn placement

Placement only moves a floating window; the size is always the client's
own (the first configure stays 0x0, so apps restore their sizes and
dialogs open natural).

- A class seen before opens where its last window closed, relative to
  that monitor, applied on the monitor it opens on. Every close counts,
  however the client tears down.
- Otherwise (no memory, or the spot is taken) it takes the least-overlap
  spot among the workspace's 24 topmost windows, preferring center: a
  lone window stays centered.
- A window born maximized gets its remembered windowed box as its
  restore: the one size the plugin supplies.
- A fixed-size toplevel (min == max: a dialog, a splash) keeps the
  compositor's centering and is never remembered.
- A window that chose its spot (X11 geometry, a dialog anchored to its
  parent) keeps it while free.
- Size limits, fullscreen and maximize state (a pending request or a
  rule's grant included) and override-redirect surfaces stay
  authoritative.

## Snap

During a floating move, nearby window and work-area edges pull within
`snap_distance`. Reaching one output edge (within `edge`) previews that
half; two edges, that corner's quarter. Releasing the button or `Super`,
whichever comes first, commits. Previews honour size limits and keep the
chosen edge anchored; the gap between two outputs arms nothing.

A window filling its slot is a tile: square corners, no shadow or
border, its surface the whole slot, so tiles meet the screen edges and
each other with no seam. A slot splits the work area on a whole pixel,
the right or bottom one taking the odd one. A drag or resize lifts the
tile and gives its corners, shadow and border back; a client whose
limits keep it smaller stays a rounded float. The look sits at the
layout's override priority, so rules setting `rounding`, `no_shadow` or
`border_size` still win. A reload forgets snapped windows (maximized
ones are adopted again).

Turn off Hyprland's `general:snap` so the two don't compete.
