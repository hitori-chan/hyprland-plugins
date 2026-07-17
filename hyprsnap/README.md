# hyprsnap

Awesome's `awful.mouse.snap`, both behaviors during every floating
move-drag. (Spawn placement lives in `hyprplace`.)

- **Magnetism** — window edges pull flush to the screen, the workarea and
  the other windows when within `snap_distance`.
- **Aerosnap** — the cursor at a screen edge arms that half; at two edges,
  that corner's quarter — outline preview, committed on drop. Either
  release order works: the button, or Super first.

Keep the native `general:snap` off: two magnets pull to different spots.

## Config

| key | what | default |
|---|---|---|
| `plugin:hyprsnap:edge` | px from a screen edge that arms a zone | 16 |
| `plugin:hyprsnap:snap_distance` | px of magnetic pull | 8 |
| `plugin:hyprsnap:col_frame` | armed zone outline | `32d6ff` |
