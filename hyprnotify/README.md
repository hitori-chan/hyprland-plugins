# hyprnotify

Awesome's `naughty`, native: the compositor is the
`org.freedesktop.Notifications` daemon (spec 1.3). No external process, no
layer surface — the renderer draws the cards top-right on the focused
monitor.

1. **Cards** — `#131313` under a 1px frame with 1px rounding; app-name
   kicker in muted letterspaced caps, bold title, body, a 4px value bar
   for the `value` hint (the volume/brightness OSD), icons capped at
   64px. Newest on top; `replaces_id` updates a card in place, keeping
   its slot. The frame warms under the pointer; critical cards take the
   urgent color and never expire.
2. **Images** — `image-data` pixmaps and file paths (`file://` too),
   decoded by hyprgraphics. Wide images (aspect ≥ 1.5) render card-width
   as a cover-cropped hero, so a screenshot notification previews the
   shot itself. Iconless cards draw a random face from
   `fallback_icon_dir` — rolled per card, held across in-place replaces:
   a volume sweep keeps its face, the next sweep gets another.
3. **Clicks** — left emits `ActivationToken` (a compositor-minted
   xdg-activation token, so the sender can raise itself), invokes the
   default action, then dismisses. Right dismisses; middle sweeps the
   visible stack. The cards own the pointer over them — hover never
   leaks to the window beneath.
4. **DND** — `hl.plugin.hyprnotify.suspend()` toggles naughty.suspend
   parity: arrivals collect silently with timeouts held; resume renders
   the queue newest-first on fresh timeouts. `hyprctl hyprnotify count`
   answers the pending total (the lockscreen bell reads it).
5. **Bounds** — `max_notifs` caps the model: overflow evicts the oldest
   non-critical card with `NotificationClosed`, critical only when
   nothing else is left.

Cards never render above the lockscreen, and input listeners guard and
reset there first. Colors, fonts and metrics arrive from theme.lua via
`plugin:hyprnotify:*` values; the C++ defaults mirror the theme.
