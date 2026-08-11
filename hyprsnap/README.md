# hyprsnap

Magnetic and edge/corner snapping during floating move drags.

- Nearby window and workarea edges pull together within `snap_distance`.
- Reaching one output edge previews a half; reaching two previews that corner's
  quarter. Release commits the preview regardless of whether the pointer button
  or `Super` is released first.
- Preview and commit honor the client's current size limits and keep the chosen
  edge anchored. Gaps between outputs do not arm a zone.

Disable Hyprland's native `general:snap` to avoid competing policies. Spawn
placement belongs to `hyprplace`.

| Key | Purpose | Default |
|---|---|---|
| `plugin:hyprsnap:edge` | distance from an output edge that arms a zone | 16 |
| `plugin:hyprsnap:snap_distance` | magnetic pull distance | 8 |
| `plugin:hyprsnap:col_frame` | preview outline | `ff9acbff` |
