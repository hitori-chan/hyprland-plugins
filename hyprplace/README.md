# hyprplace

Spawn placement for floating windows. No config.

A new window is placed by the first rule that lands on free space:

1. **Where the app's last window closed** — per class, persisted across
   relogs (`$XDG_STATE_HOME/hyprplace/lastspot.tsv`) — when that spot is
   free. This is for the app coming back: a second window of a class
   already on screen (a dialog, Telegram's Instant View webview) skips the
   memory and places fresh.
2. **The spot that overlaps the other windows the least** — KWin's default.
   A lone window keeps the compositor's centered spot, a busy screen fills
   the gaps, a full one lands where it hides the least. No cascade, no
   center pile.

The result is clamped fully on-screen with the border visible — a
border's width of margin all around — unless the window is too big to
fit, which drops the margin on that axis. Windows that chose their own
spot (X11 geometry, dialogs anchored to a parent) keep it while it's
free; X11 override-redirect surfaces are left alone.

## Size

A genuinely **resizable** app (mpv, terminals, browsers) also reopens at
its last size, applied **once** at spawn as the initial geometry: one
ordinary configure the client follows, nothing owned or re-asserted.
Unlike the force path used before 1.4.0 (client-serial stomp + forced
configure), a client-size grant in flight — a born-fullscreen or
born-maximized window picking its own restore size — still wins, and the
client's own later resizes are never fought. A **fixed-size dialog**
(`min == max`) keeps the client's size and is never resized, so nothing
blinks. Resizability is read from the xdg-toplevel min/max hints; X11
windows manage their own geometry. The size lands in `lastspot.tsv`
alongside the position (rows written before 1.2.0 hold position only,
and warm up to full geometry after the app closes once).

Maximized windows, fullscreen windows, and floats sized to the whole
workarea cover no free space (and a workarea-covering close leaves no
spot) — the placement scan puts a new window where it overlaps them the
least.
