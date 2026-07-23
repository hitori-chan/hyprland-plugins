# hyprnotify

Android's notification system on the freedesktop spec, drawn natively: the
compositor is the `org.freedesktop.Notifications` daemon (spec 1.3) — no
external process, no layer surface. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`, `icon-static`,
`persistence`, `sound`. The skin is glass·ink (`common/theme.hpp`): frosted
graphite cards with live blur, IBM Plex Sans, superellipse corners.

Two surfaces share one card model:

1. **Popups (banners)** — glass cards top-right on the focused monitor, the
   Android anatomy: an icon column (the CONTENT avatar wearing the
   identity's 13px corner badge when both exist; identity alone otherwise;
   nothing = text-only), an "App • age" header, bold title, body, a
   progress pill for the `value` hint, and the card's actions as tinted
   text buttons. Hovering reveals the ✕; critical cards ring urgent and
   never expire. Wide images (aspect ≥ 1.5) render as a cover-cropped hero.
2. **The center** (F12, the bar's bell, `hyprctl hyprnotify center`) — two
   views, like Android:
   - The **shade** holds RESIDENT live cards, unlabeled: a banner timeout
     emits `NotificationClosed` reason 1 exactly once and hides only the
     popup — the card stays as a shade row until dismissed or acted on.
     Empty state: "You're all caught up!".
   - **History** behind the ⏱ button holds dismissed/app-closed entries
     only ("No history" when empty).
   - Every row is the same two-state card: collapsed = icon + bold
     "title • age" + the newest body line (+ progress) + a 24Ø chevron;
     expanded = header/age line, title, 4-line body, progress, and the
     notification's ORIGINAL actions — history included (best-effort
     `ActionInvoked` with the original id; the entry is consumed). Live
     arrives expanded and auto-folds with its banner; recall = left click,
     delete = right click, always gestures.
   - ≥2 same-app rows fold into the three-state group model in BOTH views:
     digest (identity icon · "App • N • age" · count pill · ≤2 indented
     preview lines) → segmented 1-line children → per-child full expand.
     Fold state resets when the center closes.
   - The bottom bar: ⏱ (accent-lit in history) · a context-sensitive
     Clear button ("Clear all" dismisses the shade into history; "Clear
     history" wipes it; greys when its target is empty) · ⊖ DND
     (accent-lit while on). The wheel pages the list — captured only
     inside the panel; esc closes.

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
(live/kept/dnd/center — the badge). `hyprctl hyprnotify
{count,history,recall,center,state,clear}`; `hl.plugin.hyprnotify.
{suspend,recall,center}()`.

Markup stays the whitelisted Pango subset with the literal-`<`/`&` rescue;
`<a href>` opens via `xdg-open`; `<img src>` renders a thumbnail row;
`sound-file`/`sound-name` play through `sound_command`. Cards never render
above the lockscreen, and input listeners guard and reset there first.
Colors, fonts and metrics arrive from theme.lua via `plugin:hyprnotify:*`;
the C++ defaults ARE the glass·ink tokens.
