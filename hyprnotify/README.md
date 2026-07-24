# hyprnotify

Android's notification system on the freedesktop spec, drawn natively: the
compositor is the `org.freedesktop.Notifications` daemon (spec 1.3) — no
external process, no layer surface. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`persistence`, `sound`. The skin is glass·ink (`common/theme.hpp`): frosted
graphite cards with live blur, IBM Plex Sans, superellipse corners.

Two surfaces share one card model:

1. **Popups (banners)** — glass cards top-right on the focused monitor, the
   Android anatomy: the sender/app IDENTITY leads on the LEFT (a rolled
   fallback face from `fallback_icon_dir` when a card is iconless), a
   distinct CONTENT image rides the RIGHT thumbnail — a wide one (aspect ≥
   1.5) goes hero, full-width — then an "App • age" header, bold title,
   body, a progress pill for the `value` hint, and the card's actions as
   tinted text buttons. Hovering reveals the ✕; critical cards ring urgent.
   Without an explicit `expire_timeout` a card sticks until dismissed — a
   message waits to be read — unless it declares itself ephemeral (low
   urgency, `transient`, a `value` card): those run `timeout_low`.
2. **The center** (F12, the bar's bell, `hyprctl hyprnotify center`) — one
   scroll, three lifecycle sections drawn top to bottom, each only when
   non-empty:
   - **Urgent** — live critical cards, pinned to the top.
   - **Waiting** — live normal cards, still alive and counting unread;
     conversations (fd.o category `im.*`/`call.*`) sort atop.
   - **Earlier** — history: dismissed / app-closed / expired entries, dimmed.
   Opening the center ABSORBS the popped banners into Waiting (they park as
   shade rows, no dismiss), so closing never re-pops them; the empty shade
   says "You're all caught up!". Every card folds two ways: a single row is
   collapsed (icon + bold "title • age" + the newest body line + a 24Ø
   chevron) ⇄ open (header/age, title, body, progress, and the
   notification's ORIGINAL actions — Earlier included, best-effort
   `ActionInvoked` with the original id, the entry consumed). ≥2 same-app
   cards fold digest (identity icon · "App • N • age" · count pill · ≤2
   preview lines) ⇄ open (every child fully readable). Fold state resets
   when the center closes.
   - Verbs: left acts (default action → dismiss); right dismisses → Earlier
     (Earlier deletes); the chevron folds a single; a section header's ✕
     clears that section (Urgent/Waiting → Earlier, Earlier wipes). A left
     click on an Earlier body recalls it (fresh id, original age). The
     footer is ⊖ DND (accent-lit while on) and "Clear all" — the global
     sweep of live AND history (greys when both are empty). The wheel pages
     the list — captured only inside the panel; esc closes.

Model rules: `x-canonical-append` joins same app+summary into one growing
conversation card (~8KB, oldest lines drop; one history entry per
conversation); the OSD id band 9990-9999 replaces in place and never
appends, groups, or retires; critical bypasses DND; `ignore_dbusclose`
gates only the bus `CloseNotification` path (user dismissals and expiry are
untouched); `transient` and progress cards vanish entirely on expiry;
`max_notifs` overflow evicts the oldest non-critical into history.
Grouping keys on app identity (`desktop-entry`, else the app name).

The bar's bell talks over the bus: the `org.hitori.hyprnotify` interface on
the Notifications object carries `Toggle` (the center) and a `State` signal
(live/kept/dnd/center — the badge counts the shade, never history, the DND
queue or the OSD band). `hyprctl hyprnotify
{count,history,recall,center,state,clear}`; `hl.plugin.hyprnotify.
{suspend,recall,center}()`.

Markup stays the whitelisted Pango subset with the literal-`<`/`&` rescue;
`<a href>` opens via `xdg-open`; `<img src>` renders a thumbnail row;
`sound-file`/`sound-name` play through `sound_command`. Cards never render
above the lockscreen, and input listeners guard and reset there first.
Colors, fonts and metrics arrive from theme.lua via `plugin:hyprnotify:*`;
the C++ defaults ARE the glass·ink tokens.
