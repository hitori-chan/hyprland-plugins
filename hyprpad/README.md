# hyprpad

In-process touchpad policy with no config and no udev or shell helper.

- The touchpad disables while a physical USB or Bluetooth mouse is present and
  enables again when the mouse disappears. Aquamarine device events drive a
  400 ms coalesced recheck.
- `hl.plugin.hyprpad.toggle()` manually flips the current state until the next
  hotplug or configuration reload.
- State changes use Hyprland's in-process device configuration path.
- Feedback is an asynchronous private OSD notification with distinct enabled,
  disabled, and not-found states and a native touchpad identity.
