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

Floating size is the client's own, always — as in awesome. The close-box
is remembered whole (position + size land in `lastspot.tsv`; rows written
before 1.2.0 hold position only) but only the position is ever applied: a
self-remembering app (Firefox) restores its size itself, a content-sizer
(mpv) opens at its content's size, and a fixed-size dialog is never
touched. Reimposing the remembered size was tried and dropped — a Wayland
client obeys any sized configure, so even an unforced one dictates, and a
class-shared memory row then clips same-class webviews to the main
window's box.

Maximized windows, fullscreen windows, and floats sized to the whole
workarea cover no free space (and a workarea-covering close leaves no
spot) — the placement scan puts a new window where it overlaps them the
least.
