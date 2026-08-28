# hyprplace

Persistent placement for floating windows. No config.

On spawn, the plugin first tries the class's last free geometry from
`$XDG_STATE_HOME/hyprplace/lastspot.tsv`. A second window of the same class
uses least-overlap placement instead of stacking on the first. When no saved
spot is usable, least-overlap chooses the position that covers the least other
window area.

Resizable xdg-toplevels can receive their remembered size in the initial
configure. Client size limits, fullscreen/maximize state, pending native
requests, rules, parent-anchored dialogs, fixed-size dialogs, X11 geometry, and
override-redirect surfaces remain authoritative. Final geometry is constrained
to the current workarea when the client permits it.

The persisted store has fixed file, row, key, and entry bounds; malformed or
oversized state is ignored.
