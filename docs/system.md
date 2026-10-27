# System

The `system` module: the display and keyboard lights, volume and
microphone, and the touchpad. Its levels and switches show in the
[control center](notify.md#the-control-center); its keys answer with a
capsule.

## Lights

The display's brightness comes from `/sys/class/backlight` (the firmware
device first, then platform, then raw, as systemd orders them), the
keyboard light from the first LED named `*kbd_backlight`. Both step in
5 % of their range (the display never below a raw level of 2, the
keyboard down to off) and write through logind's
`Session.SetBrightness`: no root, no udev rule.

One write runs per light at a time; presses meanwhile add up into the
next. A device that is missing or stops answering is looked up again on
the next press or when the control center opens (a late driver, a GPU
switch); with none, the press answers "unavailable". A light of ten
levels or fewer (a ThinkPad's keyboard: off, low, high) steps a level at
a time, and its slider snaps to them.

### The keyboard in the dark

While every display is off (DPMS: an idle daemon's blank,
`hl.dsp.dpms`), the keyboard light is off too; the first display back
restores its level exactly. A choice made in the dark (a key, the
slider) stands. A locked screen keeps its light: a password is typed in
the dark.

### Follow the room

An option on the keyboard light's menu, off until chosen. The light is
on in a dark room and off in daylight, as iio-sensor-proxy's ambient
light sensor reads it; the sensor is claimed only while the option is
on.

- Dark is under 20 lux, light over 60, and a new reading must hold 5 s
  before the light follows, so a hand over the sensor never flickers it.
- On is the level you last chose (50 % before any).
- A choice by hand holds until the room actually changes; dark displays
  win over the room.
- A sensor that reports no lux has no scale to judge by: the option stays
  dim.

## Volume and microphone

`wpctl` against the default sink and source. It reads the current value
and writes the new one in its own process, so two sets at once would
lose a step: each device runs one set at a time, then a readback, and
presses meanwhile accumulate (steps sum, mute toggles fold to their
parity).

- Raising stops at 100 %, but a volume already past it (set elsewhere)
  is never pulled down; the capsule and the slider show the true
  percent over a full bar.
- The volume slider's right click lists the outputs and makes one the
  default.
- **A headset's own buttons**: a Bluetooth headset that sets its volume
  itself (AVRCP absolute volume) presses no key. BlueZ announces the
  change on the system bus, the default sink is read back, and the
  capsule shows only a level or a mute that actually moved. A headset
  without absolute volume sends volume keys, which reach your binds.

## Touchpad

The touchpad turns off while an external (USB or Bluetooth) mouse is
present, and back on when it is unplugged.

- A pointer counts as a mouse once it moves or clicks: a receiver with
  only a keyboard paired, a gaming keyboard's mouse interface or a
  presenter never does. With an external trackpad too, the policy drives
  the built-in one.
- `touchpad_toggle` (`XF86TouchpadToggle`) and the control center's
  switch flip it by hand; the flip holds until a mouse or trackpad is
  plugged or unplugged (a virtual pointer, from remote input or
  automation, is neither).
- The switch writes the compositor's per-device config (`hl.device`),
  so a config reload keeps it. A reload re-checks silently: a capsule
  answers only a change you can see, or the manual flip.

## Feedback

Every action answers with a capsule at the top center: the glyph, a
level track and its number, or words; a muted volume or microphone reads
"Muted" or "Off" in the urgent colour. Capsules replace in place (one
per control), last 1.2 s (the touchpad 1.5 s), and are feedback, never
notifications: no center row and no dot.

While the center is open its sliders show the change instead, and no
capsule covers them; opening the center drops a live capsule. The
battery's critical alert shares the capsules' id band but is a real
alert: it waits in the center like any banner.
