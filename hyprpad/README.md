# hyprpad

The old awesome touchpad module. No config. Fully in-process: no udev
monitor, no spawned processes.

1. **Auto** — the touchpad turns off while an external (USB/Bluetooth)
   mouse is present and back on when it's unplugged. Hotplug rides the
   compositor's own device signals (aquamarine `newPointer` + per-device
   destroy); a 400ms settle timer coalesces the burst one plug produces.
   External = a non-virtual, non-touchpad pointer with libinput bus type
   USB/Bluetooth; the touchpad is the compositor's own `m_isTouchpad`
   entry, not a name match.
2. **`hl.plugin.hyprpad.toggle()`** — manual flip (XF86TouchpadToggle).
   Holds until the next hotplug or config reload re-checks.
3. The flip is an in-process `hl.device({...})` eval — the same code
   `hyprctl eval` reaches, minus the fork and socket round-trip. It writes
   the per-device config store; a reload wipes that runtime state, which
   the plugin catches via `config.reloaded`.
4. Feedback is one async D-Bus Notify (replaces-id 9991, in place) on the
   plugin's own session-bus connection: `enabled` / `disabled` /
   `not found`.
