# hyprnotify

Android's notification system on the freedesktop spec, drawn natively: the
compositor is the `org.freedesktop.Notifications` daemon (spec 1.3) — no
external process, no layer surface. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`persistence`, `sound`. The skin is glass·ink (`common/theme.hpp`): frosted
graphite cards with live blur, IBM Plex Sans, superellipse corners.

Two surfaces share one card model:

1. **Popups (banners)** — glass cards top-right on the focused monitor, the
   Android anatomy: ONE icon column on the left, Android's conversation
   container — the AVATAR leads (the card's content image, which for a chat
   is the sender's face; a rolled fallback face from `fallback_icon_dir`
   when a card is iconless) and the app IDENTITY rides its bottom-right
   corner as a badge, so one column says both who sent it and which app
   carried it. A wide content image (aspect ≥ 1.5) goes hero, full-width
   instead. Then an "App • age" header, bold title, body, a progress pill
   for the `value` hint, and the card's actions as tinted text buttons.
   Hovering reveals the ✕ **and holds the timeout** — a banner never expires
   out from under the pointer reading it, and the clock restarts when the
   pointer leaves. Critical cards ring urgent.
   Without an explicit `expire_timeout` a normal card runs `timeout_normal`
   (5s) and then RETREATS to the shade — the popup goes, the card stays,
   the shade is the safety net. Critical cards are the exception: they stick
   as a banner until dismissed. Ephemerals (low urgency, `transient`, a
   `value` card) run the shorter `timeout_low`. To keep a chatty app from
   stacking the screen, `coalesce_popups` (on by default) holds it to ONE
   live banner: while its popup is up, further non-critical arrivals from it
   land silent and resident in the center (folded, badge-counted), and the
   next one pops fresh once that banner retreats — critical always shows.
2. **The shade** (F12, the bar's bell, `hyprctl hyprnotify center`) — ONE
   list of live cards, no lifecycle sections and no history view: a
   dismissed card is gone, exactly as on Android, and there is no recall.
   Opening it ABSORBS the popped banners (they park as shade rows, no
   dismiss), so closing never re-pops them; empty, it says "You're all
   caught up!".
   - **Ranking** is Android's, minus the dividers: critical, then
     conversations (fd.o category `im.*`/`call.*`), then normal, then
     silent — newest first inside each tier.
   - **Bundling** follows `GroupHelper.AUTOGROUP_AT_COUNT`: an app's cards
     collapse into one digest (identity icon · "App • N • age" · count pill
     · ≤2 preview lines, each wearing its own sender's face) only at FOUR or
     more; below that every card stands alone. Conversations never bundle —
     each chat keeps its own card, and its open row runs to Android's
     MessagingStyle depth (~7 messages) where an ordinary card gets four.
   - **Rows open by default.** An expansion budget walks the page from the
     top and opens each row while the panel still has room (the top row
     always opens — Android's one guarantee; the desktop shade is taller, so
     we keep going), then folds the overflow. A row whose open form shows
     nothing new gets no chevron at all. The panel runs to the height the
     monitor leaves below `offset_y`; what still overflows becomes wheel
     paging, with "▴" / "▾ N" cues.
   - **Verbs — left reads.** A left click anywhere on a row (the chevron
     included) folds it open ⇄ shut and does nothing else, so the most
     common intent has the biggest target and no gesture is destructive by
     accident. The card's primary action moves INTO the open row as its lead
     button, beside its other actions; a button acts and dismisses (unless
     `resident`). Right dismisses, middle sweeps. On a bundle: left expands,
     right (or the header ✕) dismisses the whole app. The footer is ⊖ DND
     (accent-lit while on) and "Clear all". Esc closes; a click outside
     closes. Fold state resets when the shade closes.

Model rules: the **conversation merge** (Android's MessagingStyle) joins one
chat's messages into one growing card (~8KB, oldest lines drop) — a fresh
Notify whose app + summary matches a live card rides the replace path with
the bodies joined, triggered by the `im.*`/`call.*` categories or by
`x-canonical-append`; the OSD id band 9990-9999 replaces in place and never
appends or groups; critical bypasses DND; `ignore_dbusclose` gates only the
bus `CloseNotification` path (user dismissals and expiry are untouched);
`transient` and progress cards vanish entirely on expiry; `max_notifs`
overflow evicts the oldest non-critical. Grouping keys on app identity
(`desktop-entry`, else the app name).

The bar's bell talks over the bus: the `org.hitori.hyprnotify` interface on
the Notifications object carries `Toggle` (the shade) and a `State` signal
(live/kept/dnd/center — the badge counts the shade, never the DND queue or
the OSD band). `hyprctl hyprnotify {count,center,state,badge,clear}`;
`hl.plugin.hyprnotify.{suspend,center}()`.

Markup stays the whitelisted Pango subset with the literal-`<`/`&` rescue;
`<a href>` opens via `xdg-open`; `<img src>` renders a thumbnail row;
`sound-file`/`sound-name` play through `sound_command`. Cards never render
above the lockscreen, and input listeners guard and reset there first.
Colors, fonts and metrics arrive from theme.lua via `plugin:hyprnotify:*`;
the C++ defaults ARE the glass·ink tokens.
