# hyprnotify design study v4

Open `index.html` directly or serve this directory with a static HTTP server.
The demo is self-contained and does not depend on `/tmp`.

The preview starts in the checked-in Hyprland style:

- right-edge 360px notification island, 30px top offset, live blur, and the current graphite/cyan palette;
- source notification rows, opaque AOSP conversation badge, real Telegram/qBittorrent identity icons, screenshot hero media, bar/tray, and semantic OSD sample;
- `pixel-avatar.png` is a bounded crop of the supplied Pixel notification's contact image; the Telegram application badge remains a separate opaque identity layer;
- pointer-only center interaction, 500ms long-press management, inline reply, right-click dismissal, compact DND, and Clear all.

The only comparison control is the small two-state switch above the preview. Its
Pixel 7 reference mode is drawn from `/tmp/aosp_notification.jpg` and
`/tmp/aosp_hold_menu.jpg`:

- 2×2 rounded quick-setting tiles with Wi-Fi identity and network subtitle;
- dark blue/teal tonal surfaces over the supplied wallpaper;
- large conversation pill with avatar, opaque Telegram badge, text actions, and snooze control;
- history / Clear all / notification-settings footer;
- long-press channel management with Priority, selected Default, Silent,
  Dismiss, Done, and a settings gear.

The native chevron and kebab affordances are not reintroduced. Secondary
notification management remains a long-press behavior. This is a static visual
study only; it does not load, deploy, or modify a live plugin.
