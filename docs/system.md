# System

The `system` module: the display's brightness, volume and microphone,
and the touchpad. Its keys answer with a capsule. Bluetooth and the
keyboard light are left to their own tools (`bluetoothctl`,
`brightnessctl`).

## Brightness

The display's backlight comes from `/sys/class/backlight` (the firmware
device first, then platform, then raw, as systemd orders them). It steps
in 5 % of its range, never below a raw level of 2, and writes through
logind's `Session.SetBrightness`: no root, no udev rule.

One write runs at a time; presses meanwhile add up into the next. A
device that is missing or stops answering is looked up again on the next
press (a late driver, a GPU switch); with none, the press answers
"Unavailable".

## Volume and microphone

`wpctl` against the default sink and source. It reads the current value
and writes the new one in its own process, so two sets at once would
lose a step: each device runs one set at a time, then a readback, and
presses meanwhile accumulate (steps sum, mute toggles fold to their
parity).

- Raising stops at 100 %, but a volume already past it (set elsewhere)
  is never pulled down; the capsule shows the true percent over a full
  bar.
- **A headset's own buttons**: a Bluetooth headset that sets its volume
  itself (AVRCP absolute volume) presses no key. BlueZ announces the
  change on the system bus, the default sink is read back once
  PipeWire has taken it (250 ms after the last announcement: PipeWire
  itself waits 200 ms), and the capsule shows only a level or a mute
  that actually moved. A headset without absolute volume sends volume
  keys, which reach your binds.

## Touchpad

The touchpad turns off while an external (USB or Bluetooth) mouse is
present, and back on when it is unplugged.

- A pointer counts as a mouse once it moves or clicks: a receiver with
  only a keyboard paired, a gaming keyboard's mouse interface or a
  presenter never does. With an external trackpad too, the policy drives
  the built-in one.
- `touchpad_toggle` (`XF86TouchpadToggle`) flips it by hand; the flip
  holds until a mouse or trackpad is plugged or unplugged (a virtual
  pointer, from remote input or automation, is neither).
- The flip writes the compositor's per-device config (`hl.device`),
  so a config reload keeps it. A reload re-checks silently: a capsule
  answers only a change you can see, or the manual flip.

## Feedback

Every action answers with a capsule at the top center: the glyph, a
level track and its number, or words; a muted volume or microphone reads
"Muted" or "Off" in the urgent colour. Capsules replace in place (one
per control), last 1.2 s (the touchpad 1.5 s), and are feedback, never
notifications: no center row and no dot; opening the center drops a live
one. The battery's critical alert shares the capsules' id band but is a
real alert: it waits in the center like any banner.
