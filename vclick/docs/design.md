# Design

How `vclick` works and why. The command is in the
[reference](reference.md), its check in [testing](testing.md).

## Input without the seat

A real click goes through the seat: pointer focus moves to the surface
under the cursor, and the app hears enter, motion, button and leave.
vclick never touches the seat. It finds the surface under the point with
the compositor's own hit test (popups, then subsurfaces, else the main
surface) and sends enter, motion, clicks or wheel steps, and leave
straight to all of the target app's `wl_pointer` objects. The seat's
focus, held buttons, constraints and cursor stay the user's; an app's
cursor requests are refused unless it holds the real pointer focus
([the cursor image](#the-cursor-image)).

A jump sends the input in one burst, a millisecond apart: enter, motion,
then the clicks or the step. A glide sends the same from the pointer it
brought there ([the glide](#the-glide)).

- A click is a press and release in one burst, so nothing stays held.
  A button stays down only between `press` and `release`
  ([a held button](#a-held-button)). What follows is the mode's.
- A scroll is one frame per step, as a wheel sends it: source `wheel`,
  `axis_value120` 120 (or `axis_discrete` 1 to an older pointer),
  direction `identical`, then `axis` 15. One step is one detent, never
  scaled by `input:scroll_factor`, so a script scrolls the same on every
  setup. Signs follow the protocol (down and right are positive), as a
  real wheel's through Hyprland; GTK 3.24 calls a discrete +x step
  "left", for a real tilt wheel too.

## The modes

- `mouse` (default): a request glides unless it says `jump`; the app's
  pointer stays where the input left it (the app keeps the hover: a
  tooltip, a hover menu, a remote desktop's cursor), and the cursor image
  stays drawn there. The next input into another app leaves this one
  first; `hide` takes the mouse away (a leave, the cursor goes).
- `tap`: a request jumps unless it says `glide`; the app's pointer leaves
  after it and no cursor stays (a glide's shows while it moves, then
  fades). Switching to `tap` ends the hover and hides the cursor.

`glide` or `jump` picks only the way there; what stays after is the
mode's.

## The glide

A jump teleports the app's pointer, and through a remote desktop the
remote cursor jumps too. A glide moves there as a hand would, then acts:

- **Path**: a cubic Bézier from where vclick's pointer last was in the
  window (else its centre) to the point. The inner control points sit at
  a third and two thirds of the line, so progress follows the speed
  profile, and bow sideways by `bow_mean` ± `bow_sd` of the length (at
  most 25 %) to a random side, or the other if the window's edge would
  cut it. Control points stay inside the window, so the whole path does.
- **Time**: Fitts's law, a + b · log2(D / W + 1) ms, 40 ms to 1 s, with
  W the target's size: 32 px unless the request names it (`size:<px>`;
  vclick can't see the target).
- **Speed**: bell-shaped, t^α (1 − t)^β with α + β = 4, peaking at `peak`
  (centred is a reaching arm's minimum jerk; people peak early).
- **Overshoot**: a move of ≥ 300 px passes the target `overshoot_p` of
  the time, by about `overshoot` of D (3–40 px) in 85 % of its time,
  then corrects straight back (60–300 ms).
- **Pace**: one motion every 1000 / `rate_hz` ms (4–16 ms) on the event
  loop. A surface boundary on the way gets a leave and an enter, as a
  real pointer's does. The last motion lands exactly on the point.
- **Dwell**: a rest of `dwell_ms` ± 30 % (≤ 400 ms) before the click or
  scroll; a move has none.

Each tick is checked afresh. If the window closed or was hidden, or the
compositor is shutting down, the glide stops and the app's pointer
leaves (the lockscreen is no reason: [under the
lockscreen](#under-the-lockscreen)). If the user's pointer came into the
app, the seat has its pointer: the glide ends, a click or scroll goes on
as a jump ([the same app](#the-same-app)), a move stops; `status` says
which. One glide at a time and nothing else meanwhile; unloading ends
it.

### The user's pace

The defaults are a typical mouse user's. `learn.py record` reads the
cursor position from the compositor's socket (~250 Hz) and the pointer
devices' raw events for exact report and click times (never a keyboard;
it grabs nothing). `learn.py fit` splits them into movements and keeps
those ending in a click within 1.2 s, Fitts's own case. It fits a and b
by least squares through the medians of 8 equal-count difficulty bands
(medians shrug off a hesitation; trimming slow moves would drop mostly
long ones and flatten b). Bow, peak, overshoot (moves ≥ 300 px) and
dwell are the moves' own; the rate is the device's report interval.

## The cursor image

The compositor refuses an app's cursor (`wl_pointer.set_cursor`, a
`cursor-shape-v1` shape) unless the app holds the real pointer focus, so
the seat never shows a glide. vclick hooks the seat manager's
`onSetCursor` to keep each app's latest image (the original still
decides), hears its named shapes, and draws the cursor itself at the
app's pointer:

- the app's own image; or its named shape (`default` until it names one)
  from the session's XCursor theme (`XCURSOR_THEME`, `XCURSOR_SIZE`,
  `XCURSOR_PATH`, as the compositor reads them; the theme's `Inherits`,
  then `default`; an older X name such as `xterm` for a theme without
  the CSS ones), sharp at the highest output scale; animated shapes
  animate; or nothing where the app hid its cursor (a remote desktop
  viewer draws the remote one); the built-in arrow only where the theme
  has no such shape;
- right after the target window's surfaces, clipped to it, so a window
  above covers it as it covers the app;
- never in a capture (screencopy, `grim -T`);
- never once the user's pointer has come into that app: the app's one
  pointer is theirs then. It doesn't return when they leave; the next
  input glides from where their pointer left the window, and the cursor
  shows again with it;
- where the window is drawn, so it follows a workspace slide;
- in mouse mode, from the input until the next input, `hide`, the
  user's pointer entering, the window closing or the switch to `tap`; in
  tap mode, during a glide and 1.2 s after, then fading over 0.3 s.

One cursor, as one mouse: in the window of the last input (`where`).
A shape is read once (a file, at most 256 px a side and 64 frames) and
kept until the theme, its size or the scales change. Without the hook
(the symbol gone after a compositor change), apps' own images are not
drawn; `image` says so.

## The hover

A `move`, and in mouse mode every input, ends without a leave: the app
keeps the hover, and its next input there needs no second enter. The
hover ends when the user's pointer changes surface (the seat sends every
app's pointer a leave), at vclick's next input into another app, at
`hide` or the switch to tap, and at unload. A move into the app under
the user's mouse is refused: that pointer is theirs.

## A held button

`press` holds a button down until `release`; a `move` meanwhile drags it.
The app hears one press, the motions, and the release: its implicit
grab, so every motion goes to the surface the press began in, in its
coordinates, wherever the point is, and no surface is crossed. The seat
never learns of it. Two things an app may make of a held button would
reach the user's real mouse, so neither can start:

- An interactive move or resize (a client-side title bar dragged):
  Hyprland checks the request's serial against the presses it recorded,
  and vclick strikes its press from that record at once.
- A drag-and-drop (a link, a file, a selection dragged out):
  `wl_data_device.start_drag` takes no checked serial, and Hyprland
  drives the drag with the user's pointer, dropping where they next let
  a button go. vclick hooks `initiateDrag`: one from the app holding its
  button, or let go of within a second (the app may answer the last
  motion late), is refused: the app hears its source cancelled, and the
  hold ends. The user's own drag in that app (their button down there)
  goes through. Without the hook, `press` is refused.

A drag inside the app (a selection, a slider, a scrollbar, a canvas)
needs neither and goes. A leave releases the app's held buttons (the
compositor sends the release first), and Hyprland's seat leaves every
app's pointer whenever the user's pointer changes surface, so that ends
the hold, as it ends the hover; so does the window closing or hiding. `hide`, the
switch to `tap` and unloading let go first. In tap mode the cursor shows
while the button is down and fades after the release.

## Paced scrolling

Apps read the time between wheel steps and scroll further for a fast
spin: through a remote desktop in Firefox, 20 steps at once scrolled
~115 lines, while steps 10–200 ms apart scrolled 2–3 lines each. So a
multi-step scroll is paced: the first step goes at once (or when the
glide arrives), each further one `ms` later on the event loop.

Each step is a whole input (enter, the step, and in tap mode the leave),
checked afresh: the window may have closed, hidden or moved, and the
user's mouse may have come into the app. A busy app (the user holds a
button there) is retried at the same pace for up to 1 s, then the run
stops; any other error ends it, and `status` says why. One run at a time
and nothing else meanwhile, so a script gets exactly the scroll it asked
for. Unloading ends a run.

## The same app

An app has one pointer per `wl_pointer`, not per window. When the user's
mouse is in the target's app (one Firefox process owns all its windows),
their pointer steps out of their surface for the input and back in
under their cursor: the app sees a quick leave and enter (a hover
flickers). The way back is measured from the cursor over the surface's
box, not the seat's last motion, which is stale after an enter without
one (a window moving under a still cursor, a workspace switch).

Never under a press: a leave releases the app's held buttons, so while
the user holds a button, a pointer constraint is active or a drag runs
in that app, vclick answers `error: busy`.

## The keyboard

`send_shortcut` moves the seat's keyboard focus to the window and back,
so a key the user types at that instant goes to the target and their
window blurs. vclick's `key` never touches the seat's keyboard focus,
held keys or modifiers: it sends the key to the client's own
`wl_keyboard` objects (all of them):

- An enter on the window's surface naming no held keys, the request's
  modifiers, the press, the release 1 ms later, the modifiers released,
  a leave. The layout group and lock keys are the user's, so a key means
  what it would typed. Press and release go in one burst: no key stays
  held, and no repeat starts.
- The key code comes from the compositor's resolver, as
  `send_shortcut`'s does, in the keymap every client has.
- When the user's keyboard is in the same app, that app's one keyboard
  focus is theirs: the key goes only into their very window, between its
  modifiers and theirs, and only while they hold nothing. Into another
  window of that app it answers `error: busy`.

A key is no pointer input: it goes at once in either mode with no
cursor, and is refused while a glide or paced scroll runs.

## Under the lockscreen

The lock doesn't stop vclick: requests go, and glides and paced scrolls
go on, so a script keeps working while the user is away.

- Input goes only to the named window through its app's own objects. A
  lock surface is no window, so nothing can name it, and the seat (the
  lockscreen's focus, its held keys) is never touched. The compositor
  refuses the target's own focus grabs under the lock (a popup grab, an
  activation), so what the user types, their password included, goes to
  the lockscreen only.
- A key goes with an enter and a leave: the lockscreen has the keyboard.
- No cursor is drawn: windows are not.

Locked windows get no frame callbacks, so an app that waits for one (a
browser) stops painting. A window capture (`grim -T`) still works and
makes the compositor present a frame, so a script that captures the
window it drives keeps it painting. With the screen off (DPMS) a capture
may wait until it is back on.

## Limits

- A click that opens a popup with a grab (a context menu, a `<select>`)
  gets the grab: Hyprland moves keyboard focus into the popup until it
  closes (Escape through `send_shortcut`, or a click elsewhere).
- An app may ask for activation after a click (a link opening a
  window); `misc:focus_on_activate` decides.
- A hover and a held button last only until the user's pointer changes
  surface (Hyprland's seat leaves every app's pointer then), and are
  refused in the user's own app.
- The mouse is per window: a glide starts from vclick's last spot in
  that window, not from where the cursor was drawn.
- No drag-and-drop, and no moving or resizing a window by its own title
  bar: Hyprland drives both with the user's real mouse
  ([a held button](#a-held-button)).
- Fitts's W is 32 px unless the request names the target's size: a small
  button gets a little less time than a hand would take.
- No XWayland: the compositor's hit test asserts on X11 windows, and an
  X11 client's keys go through XWayland's seat.
- A page may see a focus and a blur event around a key, as with
  `send_shortcut`.
