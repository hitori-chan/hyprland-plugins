# hyprnotify

Awesome's `naughty`, native: the compositor is the
`org.freedesktop.Notifications` daemon (spec 1.3). No external process, no
layer surface — the renderer draws the cards top-right on the focused
monitor. Capabilities advertised: `actions`, `action-icons`, `body`,
`body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`persistence`, `sound`.

1. **Cards** — `#131313` under a 1px frame with 1px rounding; app-name
   kicker in muted letterspaced caps, bold title, body, a 4px value bar
   for the `value` hint (the volume/brightness OSD), icons capped at
   64px. Newest on top; `replaces_id` updates a card in place, keeping
   its slot. The frame warms under the pointer; critical cards take the
   urgent color. Without an explicit `expire_timeout` a card sticks until
   dismissed — a message waits to be read — unless it declares itself
   ephemeral (low urgency, `transient`, a `value` card): those run
   `timeout_low`.
2. **Markup** — body and title render the whitelisted Pango subset
   (`<b> <i> <u> <span> <br>`); other tags are dropped and a stray `<`/`&`
   survives as literal text, so a markup-aware sender and a naive one both
   come out right. Malformed markup falls back to plain text.
3. **Images** — `image-data` pixmaps and file paths (`file://` too), plus
   freedesktop icon **names** for `app_icon` / `image-path` /
   `desktop-entry`, resolved against the GTK icon theme (then hicolor,
   then pixmaps). Decoded by hyprgraphics. Wide images (aspect ≥ 1.5)
   render card-width as a cover-cropped hero. Iconless cards draw a random
   face from `fallback_icon_dir`. `<img src>` in the body renders as a
   thumbnail row below the text.
4. **Actions** — non-`default` actions render as a clickable button row; a
   left click emits `ActionInvoked` and dismisses the card unless the
   `resident` hint is set. Under the `action-icons` hint the action ids
   are freedesktop icon names drawn on the buttons. The `default` action
   (and a lone action) fire on a body left click; `ActivationToken`
   precedes each invoke so the sender can raise itself.
5. **Hyperlinks** — `<a href>` renders underlined in the link color; a
   click opens the URL via `xdg-open` and leaves the card up.
6. **Clicks** — left invokes the action / opens the link / fires the
   default, then dismisses; right dismisses; middle sweeps the visible
   stack. The cards own the pointer over them — hover never leaks to the
   window beneath.
7. **Sound** — `sound-file` / `sound-name` play through a libcanberra
   player (`sound_command`, empty disables); `suppress-sound` mutes a
   single notification. The compositor has no audio backend, so this
   shells out, reaped off the event loop.
8. **DND** — `hl.plugin.hyprnotify.suspend()` toggles naughty.suspend
   parity: arrivals collect silently with timeouts held; resume renders
   the queue newest-first on fresh timeouts.
9. **History** — a closed card is retained (bounded by `max_history`)
   unless it is `transient` or a progress/OSD card;
   `hl.plugin.hyprnotify.recall()` (or `hyprctl hyprnotify recall`) pops
   the most recent back onto the stack with a fresh timeout.
   `hyprctl hyprnotify {count,history}` answer the live and retained
   totals (the lockscreen bell reads `count`).
10. **Bounds** — `max_notifs` caps the model: overflow evicts the oldest
    non-critical card with `NotificationClosed`, critical only when
    nothing else is left.

Cards never render above the lockscreen, and input listeners guard and
reset there first. Colors, fonts and metrics arrive from theme.lua via
`plugin:hyprnotify:*` values; the C++ defaults mirror the theme.
