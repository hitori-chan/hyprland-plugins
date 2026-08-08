# hyprnotify design study v5

Open `index.html` directly or serve this directory with a static HTTP server.
The demo is self-contained and does not depend on `/tmp`.

The preview starts in the checked-in Hyprland style:

- right-edge 360px notification island, live blur, current graphite/cyan palette, source rows, source footer, bar/tray, and semantic OSD sample;
- real Telegram/qBittorrent identity icons, screenshot hero media, opaque AOSP conversation badge, pointer-only interaction, inline reply, long-press management, right-click dismissal, compact DND, and Clear all;
- the native chevron and kebab affordances remain absent; secondary management is a hold gesture.

The Pixel 7 reference switch is a fixed 576×1280 canvas. Its geometry is
measured against the supplied `/tmp/aosp_notification.jpg` and
`/tmp/aosp_hold_menu.jpg` captures: status row, 2×2 quick settings, shade
anchor, notification card, management card, footer, and navigation pill all
use capture-space coordinates. On narrow browsers the complete canvas scales
as one unit instead of reflowing its internal layout.

Pixel/SystemUI assets are derived from the supplied factory ROM:

`~/Downloads/panther-cp2a.260705.006-factory-ed94a24e.zip` →
`image-panther-cp2a.260705.006.zip` → `system_ext.img:/priv-app/SystemUIGoogle/SystemUIGoogle.apk`
and `product.img:/fonts/GoogleSansFlex-Regular.ttf`.

The bundled vectors cover Wi‑Fi (`ic_wifi_3`), Bluetooth (`vd_bluetooth`),
airplane mode (`vd_airplane`), hotspot (`vd_hotspot`), mobile signal/error
(`ic_mobile_3_5_bar_error`), status battery, battery charging, volume media,
brightness, touch, snooze, priority, alert/silent notification channels,
notification settings, history, and the notification footer settings mark.
The current Hyprland preview retains the configured wallpaper and local
application/avatar assets; the Pixel canvas uses the measured blue/teal
backdrop from the supplied captures. The ROM/APK itself is not copied into
the repository. The OSD path shapes and Pixel status/management marks are
ROM-derived; their fills use the demo's current semantic palette.

This is a static visual study only; it does not load, deploy, or modify a live
plugin.
