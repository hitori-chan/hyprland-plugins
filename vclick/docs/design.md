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
change the user's cursor either.

A click goes out in one burst, timestamped a millisecond apart, and so
does each scroll step.

- A click: press and release, then the leave, so no button stays held in
  the app and a drag or an interactive move cannot start.
- A scroll: one frame per step, as a wheel sends it: source `wheel`, the
  discrete step (`axis_value120` 120, or `axis_discrete` 1 to an older
  pointer), the relative direction `identical`, then `axis` 15. One step
  is one standard detent, never scaled by `input:scroll_factor`, so a
  script scrolls the same on every setup; an app scrolls by its own step
  per detent (Firefox: a few lines). Signs follow the protocol (`axis`
  is in motion coordinates: down and right are positive), as a real wheel
  sends them through Hyprland; GTK 3.24 names a discrete +x step "left",
  a real tilt wheel's as much as vclick's.

## Paced scrolling

An app reads the time between wheel steps. A burst of steps looks like a
fast spin, and many apps scroll further for it. Through a remote desktop
in Firefox, 20 steps sent at once scrolled about 115 lines, while steps
10 to 200 ms apart scrolled 2 to 3 lines each. So a scroll of more than one
step is paced by default: the first step goes with the answer, and each
further step goes on the compositor's event loop `ms` later.

Each paced step is a whole input of its own (enter, the step, leave),
checked afresh: the window may have closed, hidden or moved, and the
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

## Limits

- A click that opens a popup with a grab (a context menu, a `<select>`,
  an app menu) gets the grab: Hyprland grants every `xdg_popup` grab and
  moves keyboard focus into the popup, so the user's typing goes there
  until it closes (Escape through `send_shortcut`, or a click elsewhere).
- An app may ask for activation after a click (opening a link in a new
  window); `misc:focus_on_activate` decides, as for any app.
- No hover and no drag. A hover has to stay, but the seat sends every
  app's pointer a leave whenever the user's pointer changes surface, and
  in the user's own app the one pointer is the user's. A drag-and-drop
  (a tab, a link, a file) is the app's call, and Hyprland then drives it
  with the user's real mouse: it follows the cursor and drops on the
  user's next real release.
- No XWayland: the compositor's hit test asserts on an X11 window.
