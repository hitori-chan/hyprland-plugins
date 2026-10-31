# Windows

The `windows` module: one state machine per window (normal, minimized,
maximized), the click and focus policy, and all placement.

## Minimize

Hyprland has no minimized state of its own: the plugin takes a
minimized window out of layout and render and remembers its fullscreen,
maximize and floating state for the restore. Focus cycling skips
minimized windows.

Minimizing the focused window focuses the workspace's most recently
focused task (awesome's autofocus): that exact window, unraised, even
when another window covers its center.

## Maximize

Maximize is per window. A floating window fills the current work area
and carries the native xdg maximized state, leaving Hyprland's
fullscreen slot alone; a tiled window takes the compositor's own
maximize. The rest of this section is about floating windows.

- A window that the app or the compositor maximized is adopted into the
  same model. A client's own unmaximize (its CSD restore button or a
  titlebar double-click) leaves maximize exactly as `Mod+M` does.
- A maximized window follows workspace, output and reserved-area
  changes, and the pointer follows the window's geometry, so a window
  that grows under a still cursor takes the next click.
- A maximized window is a [tile](#snap).
- A maximized window does not move, as in awesome's
  `mouse.client.move`: a `Mod`+drag is swallowed, and a drag the client
  starts from its own titlebar ends at once.
- The restore uses the remembered windowed box, constrained by the
  current work area and the client's size hints.
- A grouped window is neither minimized nor maximized by the plugin (its
  slot is the group's).
- A plugin reload keeps both states: minimized windows are shown again
  as the plugin stops, and the next instance adopts the windows still
  told maximized.

## Click and focus

- A plain left click raises its target. Clicking a fullscreen window
  tucks the floaters allowed over it back behind it, without rewriting
  the z-order.
- Keyboard focus raises; pointer hover does not.
- A short corpse guard eats presses aimed at a window that has just
  closed or left fullscreen, so the click does not land on the window
  below.
- `focus_next` / `focus_prev` cycle in stable arrival order (not the
  z-order that click-to-raise keeps changing); `focus_prev_here` toggles
  the two most recent windows on the current workspace. Both, and the
  snap magnet, work on an open special workspace.

### Activation: attention, not focus

The mechanism is awesome's `permissions.activate`. By default a
background app asking to be presented gets attention, never the
keyboard, so a message arriving in a chat app does not steal what you
are typing.

| Event | Result |
|---|---|
| a new window maps | it takes focus (awesome's `focus = awful.client.focus.filter` rule) |
| an app asks for activation (xdg-activation) with `misc:focus_on_activate` off, the fork's default | its task turns urgent; a task click, `Mod+U` or a tray or card click completes it |
| the focused, visible window asks | nothing (awesome never marks the focused client urgent) |
| a window on another workspace, or minimized, asks | urgency only: no focus, no workspace switch |
| a taskbar or window switcher activates a window (wlr-foreign-toplevel) | it is focused wherever it is: the pick is the user's |
| an X11 app sends `_NET_ACTIVE_WINDOW` | urgency only, always: X11 cannot authenticate the gesture, and Wine sends it on every internal focus change |

- Focus clears urgency, as in awesome.
- `Mod+U` is `hl.dsp.focus({ urgent_or_last = true })`: the urgent
  window wins over the last-window fallback, and the view follows it to
  its workspace (awesome's `awful.client.urgent.jumpto`); a minimized
  one is restored, as is any window a focus dispatch names.
- `focus_on_activate = true` (globally or by window rule) gives vanilla
  awesome: an ask on a visible window focuses it, and a Wayland ask on a
  minimized one restores and focuses it. An ask on another workspace is
  still urgency only.
- A taskbar's minimize and unminimize run through the same state machine
  as a client's own minimize button.
- Tray and notification clicks focus the app's own window from the
  compositor side, restoring it first when its only window is minimized.

Urgency for activation asks, and the X11 rule, are fork extensions:
upstream Hyprland ignores such asks.

## Spawn placement

Placement only moves a floating window; its size is always the client's
own (the initial configure stays 0x0, so apps restore their own sizes
and dialogs open at their natural size).

- A window of a class seen before opens where that class's last window
  closed, relative to the monitor it was on, applied on the monitor it
  opens on. The close is remembered however the client tears down.
- Otherwise (no memory, or its spot is taken) a window takes the
  least-overlap position among the workspace's 24 topmost windows,
  starting from, and so preferring, its centered spot: a lone window
  stays centered.
- A window born maximized gets its remembered windowed box as the
  unmaximize restore: the one size the plugin supplies.
- A fixed-size toplevel (min == max: a dialog, a splash) keeps the
  compositor's centered placement and is never remembered.
- A window that chose its spot (X11 geometry, a dialog anchored to its
  parent) keeps it while it is free.
- Client size limits, fullscreen and maximize state (a pending request
  or a rule's grant included) and override-redirect surfaces stay
  authoritative.

## Snap

During a floating move, nearby window and work-area edges pull together
within `snap_distance`. Reaching one output edge (within `edge`)
previews that half; two edges, that corner's quarter. Release commits
the preview whichever is let go first, the button or `Super`. Previews
honour the client's size limits and keep the chosen edge anchored; the
gap between two outputs arms no zone.

A window that fills its slot is a tile: square corners, no drop shadow
and no border, its surface the whole slot, so halves and quarters meet
the screen's edges and each other with nothing between (rounded corners
leave the wallpaper showing at the seams, and a border doubles every
seam). A slot splits the work area on a whole pixel, the right or bottom
one taking an odd one. A drag or a resize lifts the tile out and gives
its corners, drop and border back; a client whose limits keep it smaller
than the slot stays a rounded float. The look sits at the layout's
override priority, so window rules that set `rounding`, `no_shadow` or
`border_size` still win. A plugin reload forgets which windows were
snapped (maximized ones are adopted again).

Turn off Hyprland's own `general:snap` so the two do not compete.
