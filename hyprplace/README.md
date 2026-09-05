# hyprplace

Persistent placement for floating windows. No config.

On spawn, the plugin first tries the class's last free geometry from
`$XDG_STATE_HOME/hyprplace/lastspot.tsv`. A second window of the same class
uses least-overlap placement instead of stacking on the first. When no saved
spot is usable, least-overlap chooses the position that covers the least other
window area.

Resizable xdg-toplevels can receive their remembered size in the initial
configure. Client size limits, fullscreen/maximize state, pending native
requests, rules, parent-anchored dialogs, X11 geometry, and
override-redirect surfaces remain authoritative. Final geometry is constrained
to the current workarea when the client permits it.

A fixed-size toplevel (min == max — a dialog or a splash) keeps the
compositor's native centered placement and never reads or writes the class
row: its transient box cannot clobber the app's remembered close-box, and
the app's box cannot steer it into a least-overlap corner (a maximized
Electron main is grant-exempt, so without the exclusion the class row would
be the splash's own, and the corner it was shoved to would be re-remembered
on every launch).

The persisted store has fixed file, row, key, and entry bounds; malformed or
oversized state is ignored.
