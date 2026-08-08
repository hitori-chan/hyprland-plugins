# hyprnotify design mixer v3

Open `index.html` directly or serve this directory with any static HTTP
server. The demo is self-contained and does not depend on `/tmp`.

The asset bundle is source-derived from this workstation and the checked-in
Hyprnotify/Hyprbar implementation:

- `telegram.png` is the installed `org.telegram.desktop` desktop-entry icon.
- `qbittorrent.png` is the installed `qbittorrent` desktop-entry icon.
- `screenshot.svg` and `bar-bell-aosp.svg` are installed Adwaita semantic
  icons.
- `bar-wifi-connected.svg`, `bar-bluetooth-active.svg`, and
  `bar-volume-high.svg` are representative StatusNotifier-style tray items;
  they are not claimed to be the live session's registered items.
- `floating.png` is the configured Hyprland floating-layout icon.
- `osd-*.svg` follows the current `hyprnotify/paint.cpp` control-icon paths
  for volume, brightness, touchpad, and battery.
- `bar-battery-pixel.svg` is a static 82% charging sample of the dynamic Pixel
  battery pill rendered by `hyprbar/battery.cpp`.
- `screenshot-preview.png` is a real nested Hyprland notification capture.
- `wallpaper.jpg` is a resized copy of the configured Hyprpaper wallpaper.

The preview has one unified control board above the stage. It covers design,
geometry, palette, row/footer/badge variants, center/OSD state, and hold
behavior, including an optional light AOSP-style OSD border; the mock bar also
demonstrates workspace, task, tray, bell, and layout interactions. It does not
load or modify a live plugin.
