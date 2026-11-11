# Design

How `vclick` works and why. The command is in the
[reference](reference.md); the check that holds it to this is in
[testing](testing.md).

## Input without the seat

A real click goes through the seat: the compositor moves its pointer
focus to the surface under the cursor and the app hears enter, motion,
button and leave there. vclick never touches the seat. It finds the
surface under the point with the compositor's own hit test (popups
first, then subsurfaces, the main surface where no input region takes
the point), and sends enter, motion, the clicks or wheel steps, and
leave straight to the target app's own `wl_pointer` objects, all of them
(an app may bind more than one). The seat's pointer focus, held buttons,
constraints and cursor stay the user's; an app's cursor requests are
refused unless it holds the real pointer focus, so the target cannot
change the user's cursor either ([the cursor image](#the-cursor-image)).

A jump sends the input in one burst, timestamped a millisecond apart:
enter, motion, then the clicks or the scroll step. A glide sends the
same input from the pointer it has already brought there
([the glide](#the-glide)).

- A click: press and release, so no button stays held in the app and a
  drag or an interactive move cannot start. What follows is the mode's
  ([the modes](#the-modes)): the pointer stays (mouse) or leaves (tap).
- A scroll: one frame per step, as a wheel sends it: source `wheel`, the
  discrete step (`axis_value120` 120, or `axis_discrete` 1 to an older
  pointer), the relative direction `identical`, then `axis` 15. One step
  is one standard detent, never scaled by `input:scroll_factor`, so a
  script scrolls the same on every setup; an app scrolls by its own step
  per detent (Firefox: a few lines). Signs follow the protocol (`axis`
  is in motion coordinates: down and right are positive), as a real wheel
  sends them through Hyprland; GTK 3.24 names a discrete +x step "left",
  a real tilt wheel's as much as vclick's.

## The modes

- `mouse` (the default) behaves as a mouse: a request glides there
  unless it says `jump`, the app's pointer stays where the input left it
  (the app keeps the hover, as under a mouse at rest: a tooltip, a hover
  menu, and through a remote desktop the remote cursor stays put), and
  the cursor image stays drawn there ([the cursor
  image](#the-cursor-image)). The next input into another app leaves
  this one first, as a mouse leaves a window; `hide` takes the mouse
  away (a leave, and the cursor goes).
- `tap` is only the input: a request jumps unless it says `glide`, the
  app's pointer leaves after it, and no cursor stays (a glide's shows
  while it moves, then fades). Switching to `tap` ends the mouse's hover
  and hides its cursor.

A request's `glide` or `jump` picks only the way there; what stays
after is the mode's.

## The glide

A jump puts the app's pointer on the point at once. An app sees that as
a teleport, and through a remote desktop the remote cursor jumps too. A
glide moves the pointer there as the user's hand would, then acts:

- The path: a cubic Bézier from where vclick's pointer last was in the
  window (else its centre) to the point. Its inner control points sit at
  a third and two thirds of the line, so the progress along it is the
  speed profile's, and off it sideways, so the path bows like a hand's
  arc: by `bow_mean` ± `bow_sd` of its length (at most 25%), to a random
  side, or the other one if the window's edge would cut the bow. The
  control points are kept inside the window, and the curve stays in
  their convex hull: so does the whole path.
- The time: Fitts's law, a + b · log2(D / W + 1) ms, 40 ms to 1 s.
  vclick can't see the target's size, so W is 32 px.
- The speed: zero at both ends and bell-shaped, t^α (1 − t)^β with
  α + β = 4, peaking at `peak` of the move. Centred, that is the
  minimum-jerk speed of a reaching arm; people peak early.
- The overshoot: a move of 300 px or more passes the target `overshoot_p`
  of the time, by about `overshoot` of D (3–40 px), in 85% of its time,
  then a short straight correction comes back (60–300 ms).
- The pace: one motion every 1000 / `rate_hz` ms (4–16 ms), on the
  compositor's event loop. A surface boundary on the way (a subsurface,
  a popup) is crossed with a leave and an enter, as a real pointer
  crosses it. The last motion lands exactly on the point.
- The dwell: the pointer rests on the target for `dwell_ms` ± 30% (at
  most 400 ms), then the click or scroll goes from there. A move has no
  dwell.

Each tick is checked afresh. If the window closed or was hidden, the
session locked or the compositor is shutting down, the glide stops and
the app's pointer leaves. If the user's pointer came into the app, the
seat has taken the app's pointer: the glide ends there, a click or
scroll goes on as a jump ([the same app](#the-same-app)), and a move
stops. `status` says which.

One glide at a time, and nothing else meanwhile. Unloading the plugin
ends it, and the app's pointer leaves.

### The user's pace

The defaults are a typical mouse user's. `learn.py record` reads the
user's own movements: the cursor's position from the compositor's socket
(about 250 Hz) and the pointer devices' raw events, for the exact time
of each report and click (never a keyboard; it grabs nothing).
`learn.py fit` splits them into movements (still, moving, still again)
and keeps those that end in a click within 1.2 s, Fitts's own case. It
fits a and b by least squares through the medians of 8 equal-count bands
of difficulty: medians shrug off a hesitation, where trimming the
slowest moves would drop mostly the long ones and flatten b. The bow,
the speed's peak, the overshoot (of moves of 300 px or more) and the
dwell are the moves' own; the rate is the device's report interval.

## The cursor image

An app sets its cursor image (`wl_pointer.set_cursor`) when its pointer
enters, and again when it wants another; the compositor refuses it unless
the app holds the real pointer focus. So the seat never shows a glide's
image, and the user would see nothing move. vclick hooks the seat
manager's `onSetCursor` and keeps each app's latest image (the original
then decides as it always does), and it hears `cursor-shape-v1`'s named
shapes. It draws that image itself at the app's pointer:

- right after the target window's own surfaces, clipped to it: a window
  above it covers the cursor where it covers the app;
- never in a capture (screencopy, `grim -T`), so a window capture shows
  the app as it is;
- never once the user's own pointer has come into that app: the app's
  one pointer is theirs then, and so is the one cursor over it. The
  cursor goes (a glide hands over to a jump), and it doesn't come back
  where it was when their pointer leaves: the app's pointer was last
  where theirs left the window, so the next input there glides from that
  spot, and the cursor shows again with it;
- where the window is drawn, its slide included, so it moves with the
  window during a workspace animation;
- in the mouse mode, from the input on and then at rest, until the next
  input moves it, `hide`, the user's pointer coming into the app, the
  window closing, or the switch to `tap`;
  in the tap mode, while a glide runs, for 1.2 s after it, then fading
  over 0.3 s. An animated image keeps going (its frame callbacks).

There is one cursor, as there is one mouse: it is in the window of the
last input. `where` says where it is.

A named shape, a hidden cursor or none gets a built-in arrow: vclick
loads no cursor theme. Without the hook (the symbol not found after a
compositor change), the arrow is all it draws; `cursor` says so.

## The hover

A `move`, and in the mouse mode every input, ends without a leave: the
app keeps the hover (a tooltip, a menu that opens on hover), and its
next input there goes from the hover, without a second enter. The hover
ends:

- when the user's pointer changes surface: the seat sends every app's
  pointer a leave (the cursor stays drawn; the next input enters again);
- at vclick's next input into another app: it leaves first;
- at `hide`, and at the switch to the tap mode;
- when the plugin unloads.

A move into the app under the user's mouse is refused: there the app's
one pointer is the user's.

## Paced scrolling

An app reads the time between wheel steps. A burst of steps looks like a
fast spin, and many apps scroll further for it. Through a remote desktop
in Firefox, 20 steps sent at once scrolled about 115 lines, while steps
10 to 200 ms apart scrolled 2 to 3 lines each. So a scroll of more than one
step is paced by default: the first step goes at once (with the answer,
or when a glide arrives), and each further step goes on the compositor's
event loop `ms` later.

Each paced step is a whole input of its own (enter, the step, and in
the tap mode the leave; in the mouse mode the pointer stays between
steps), checked afresh: the window may have closed, hidden or moved, and the
user's mouse may have come into the app.

- If the app is busy (the user holds a button there, see below), the
  step is retried at the same pace for up to 1 s. If it is still busy
  after that, the run stops.
- Any other error ends the run, and `status` says why.

One run at a time, and nothing else meanwhile, so a script gets exactly
the scroll it asked for. Unloading the plugin ends a run.

## The same app

An app has one pointer per `wl_pointer` object, not one per window. When
the user's mouse is in the same app as the target (one Firefox process
owns all its windows), that pointer steps out of the user's surface for
the input and back in under the user's cursor: the app sees a quick
leave and enter there (a hover flickers off and on). The way back is
measured from the cursor over the surface's box, not the seat's last
motion: that is stale after an enter without one (a window moving under
a still cursor, a workspace switch).

That never happens under a press: a leave releases the app's held
buttons, so while the user holds a button, a pointer constraint is
active or a drag is running in that app, vclick answers `error: busy`.

## The keyboard

A key through the seat (`send_shortcut`) moves the seat's keyboard focus
to the window, sends the key, and moves it back. Meanwhile the user's
own keys follow the focus: a key the user types at that instant goes to
the target, and their window hears a leave and an enter (a text field
blurs, a menu closes). vclick's `key` never touches the seat's keyboard
focus, held keys or modifiers. It finds the client's `wl_keyboard`
objects (through its `wl_seat` bindings: an app may bind more than one)
and sends them the key alone:

- An enter on the window's surface, naming no held keys (what the user
  holds is theirs), the request's modifiers, the press, the release a
  millisecond later, the modifiers released, and a leave. The layout
  group and the lock keys are the user's keyboard's, so a key means in
  the app what it would mean typed. The press and the release go in one
  burst: no key stays held, and the app's own key repeat never starts.
- The key code comes from the compositor's own resolver, as
  `send_shortcut`'s does, in the keymap every client was sent.
- When the user's keyboard is in the same app, that app has one keyboard
  focus, theirs. A leave and an enter around the key would blur their
  window, so the key goes only into the very window they are in, between
  its modifiers and the user's again, and only while they hold no key or
  modifier (theirs would join the request's). Into another window of
  that app (a second browser window, or a menu of the target that has the
  keyboard), the answer is `error: busy`.

A key is no pointer input: it goes at once in either mode, and no cursor
shows. Like a click, it waits for nothing and is refused while a glide
or a paced scroll runs.

## Limits

- A click that opens a popup with a grab (a context menu, a `<select>`,
  an app menu) gets the grab: Hyprland grants every `xdg_popup` grab and
  moves keyboard focus into the popup, so the user's typing goes there
  until it closes (Escape through `send_shortcut`, or a click elsewhere).
- An app may ask for activation after a click (opening a link in a new
  window); `misc:focus_on_activate` decides, as for any app.
- A hover lasts only until the user's pointer changes surface, and is
  refused in the user's own app ([the hover](#the-hover)).
- The mouse is per window, not per screen: a glide starts where vclick's
  pointer last was in its own window, so going to another window starts
  from that window's last spot, not from where the cursor was drawn.
- No drag. A drag-and-drop (a tab, a link, a file) is the app's call,
  and Hyprland then drives it with the user's real mouse: it follows the
  cursor and drops on the user's next real release.
- A glide's target is 32 px wide to Fitts's law, whatever its real size:
  a small button gets a little less time than a hand would take.
- No XWayland: the compositor's hit test asserts on an X11 window, and an
  X11 client's keys go through XWayland's seat.
- A key's app sees it focused for that instant: a page may hear a focus
  and a blur event around the key, as with `send_shortcut`.
