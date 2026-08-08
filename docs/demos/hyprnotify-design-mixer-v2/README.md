# hyprnotify design mixer v2

Open `index.html` directly or serve this directory with any static HTTP
server. The demo is self-contained and does not depend on `/tmp`.

The asset bundle is source-derived from this workstation and the checked-in
Hyprnotify implementation:

- `telegram.png` is the installed `org.telegram.desktop` desktop-entry icon.
- `qbittorrent.png` is the installed `qbittorrent` desktop-entry icon.
- `screenshot.svg` and `bar-bell-aosp.svg` are installed Adwaita semantic
  icons.
- `osd-*.svg` follows the current `hyprnotify/paint.cpp` control-icon paths
  for volume, brightness, touchpad, and battery.
- `bar-battery-pixel.svg` is a static 82% charging sample of the dynamic Pixel
  battery pill rendered by `hyprbar/battery.cpp`.
- `screenshot-preview.png` is a real nested Hyprland notification capture.
- `wallpaper.jpg` is a resized copy of the configured Hyprpaper wallpaper.

This is a visual proposal only. It does not load or modify a live plugin.
