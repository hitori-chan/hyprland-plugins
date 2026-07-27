# hyprnotify

Android's notification system on the freedesktop spec, drawn natively: the
compositor is the `org.freedesktop.Notifications` daemon (spec 1.3) — no
external process, no layer surface. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`inline-reply`, `persistence`, `sound`. The skin is glass·ink
(`common/theme.hpp`): frosted graphite cards with live blur, IBM Plex Sans,
superellipse corners.

Two surfaces share one card model:

1. **Popups (banners)** — glass cards top-right on the focused monitor, the
   Android anatomy: ONE icon column on the left, Android's conversation
   container — the AVATAR leads (the card's content image, which for a chat
   is the sender's face; a rolled fallback face from `fallback_icon_dir`
   when a card is iconless) and the app IDENTITY rides its bottom-right
   corner as a badge — AOSP's 2025 ratios off a 40dp avatar, so the app
   glyph is 16dp of the 20dp badge and the rim only 2dp — so one column
   says both who sent it and which app carried it. A wide content
   image (aspect ≥ 1.5) goes hero, full-width instead. Then an "App • age"
   header, bold title, body, a progress pill
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
   `quiet_fullscreen` (on by default) does the same for a screen that is
   spoken for: while a REAL fullscreen window owns the monitor, banners are
   held back and the card lands straight in the shade. Nothing is lost —
   residency is that safety net — and critical punches through, as through
   DND. A merely maximized window does not count.
2. **The shade** (F12, the bar's bell, `hyprctl hyprnotify center`) — ONE
   list of live cards, no lifecycle sections and no history view: a
   dismissed card is gone, exactly as on Android, and there is no recall.
   Opening it ABSORBS the popped banners (they park as shade rows, no
   dismiss), so closing never re-pops them; empty, it says "You're all
   caught up!". **Hovering the bell peeks it open** after
   `hyprbar:bell_peek_ms` (350ms, 0 = off) without costing a click: a peek
   does NOT absorb the banners, and it closes again once the pointer is on
   neither the bell nor the panel. Any click pins it, and a pinned shade is
   an ordinary one.
   - **Ranking** is Android's, minus the dividers: critical, then marked
     conversations, then the rest of the conversations (fd.o category
     `im.*`/`call.*`), then normal, then silent — newest first inside each
     tier. A silenced app ranks with the silent ones.
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
   - **Verbs — a row is its banner.** Because rows open by default, the
     left click is spent ACTING rather than revealing: clicking a row's body
     fires the card's primary (the fd.o `default`) and dismisses it unless
     `resident` — the same verb the popup has always had, and what every
     other shade does. Links open, action buttons act, and the CHEVRON is
     the only fold target. The primary is never drawn as a button: the spec
     says implementations are free not to display `default`, and a button
     would only duplicate the click. Right dismisses, middle sweeps. On a
     bundle: left expands, right (or the header ✕) dismisses the whole app.
     The footer is ⊖ DND (accent-lit while on) and "Clear all". A click
     outside closes.
   - **Acting closes the shade**, exactly as it collapses Android's. The
     primary, an action button and a body link all raise something over the
     panel you clicked in, so the panel leaves with them. Everything that
     keeps you here keeps it: dismissing, folding, the manage strip, DND,
     "Clear all", the reply field, and a `resident` card's actions — the
     spec's own way of saying the action does not take you away.
   - **Per-app rules** — what Android hides behind a long-press, and the
     thing one global DND could never say. An open row reveals a strip
     beside its chevron on hover: **⊘** silences the app (no banner, no
     sound, straight to the shade, ranked with the quiet ones — critical
     still punches through, exactly as through DND) and **★** marks the
     sender, which sorts that chat above everything but a critical card and
     lights the ring AOSP keeps hidden on the badge until you mark someone.
     A bundle's ⊘ sits by its count pill. Both rules persist across relogs
     in `$XDG_STATE_HOME/hyprnotify/policy.tsv`, both are retroactive (the
     cards already in the shade re-rank under them), and silence keys on the
     app while a mark keys on app + sender — one chat app carries many
     people.
   - **Snooze** (**◷** in the same strip, or `s`) is Android's, and it is a
     verb on one card rather than a rule: the card leaves the shade
     outright — no section, nothing to scroll past — and comes back
     ALERTING after `snooze_seconds` (15min), which is the whole point of
     asking. It stays in the model the entire time, so "Clear all" cannot
     quietly cancel a reminder, and there is no un-snooze because there is
     nothing left to click: a mis-snooze costs the interval, exactly as it
     does on the phone. Ephemerals (`transient`, progress) are refused —
     expiry takes those cards whole, so they have nothing to come back to.
   - **Inline reply.** A sender that sees the `inline-reply` capability
     adds an action keyed `inline-reply` and waits for a
     `NotificationReplied(id, text)` signal — it is how Telegram, Fractal
     and the rest offer a reply box, and a server without the capability
     gets no reply affordance offered at all. An open row with one grows a
     **Reply** chip; clicking it (or Tab on the selected row) arms a text
     field, Enter sends, Esc drops it. While a field is armed it owns EVERY
     key, because there is no keyboard focus to hand it — the same grab
     hyprbar's menubar prompt takes. Editing is append-and-backspace plus
     C-u / C-w; the field is shade-only, not on banners.
   - **Keys.** While the shade is open it owns exactly the nav set and
     nothing else: Esc closes, ↑/↓ move a selection (an accent hairline,
     paging to stay on screen), Space folds it, Enter fires the primary,
     Tab opens a reply, Delete dismisses, `m` silences the app, `s` snoozes
     the card, `p` marks the sender. A chord with ctrl/alt/super is a user
     bind passing through, and a nav key with nothing selected — or nothing
     to do, like `p` on a card that is not a chat — still belongs to
     whatever holds focus. Selection and fold state reset on close.

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
the Notifications object carries `Toggle` (the shade), `Peek(on_bell)` (the
hover) and a `State` signal (live/kept/dnd/center — the badge counts the
shade, never the DND queue or the OSD band).
`hyprctl hyprnotify {count,center,state,badge,policy,snoozed,clear}`;
`hl.plugin.hyprnotify.{suspend,center}()`.

Markup stays the whitelisted Pango subset with the literal-`<`/`&` rescue;
`<a href>` opens via `xdg-open`; `<img src>` renders a thumbnail row;
`sound-file`/`sound-name` play through `sound_command`. Cards never render
above the lockscreen, and input listeners guard and reset there first.
Colors, fonts and metrics arrive from theme.lua via `plugin:hyprnotify:*`;
the C++ defaults ARE the glass·ink tokens.
