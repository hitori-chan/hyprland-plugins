# System

The `system` module: backlight, volume and microphone, and the touchpad.
Its keys answer with a capsule. Bluetooth and the keyboard light are
left to their own tools (`bluetoothctl`, `brightnessctl`).

## Brightness

The backlight from `/sys/class/backlight` (firmware, then platform, then
raw, as systemd orders them), in `brightness_step` percent of its range
(5), never below raw level 2, written through logind's
`Session.SetBrightness` (no root, no udev rule).

One write runs at a time; presses meanwhile add into the next. A missing
or unresponsive device is looked up again on the next press (a late
driver, a GPU switch); with none, the press answers "Unavailable".

## Volume and microphone

`wpctl` on the default sink and source. wpctl reads and writes in its
own process, so concurrent sets would lose steps: each device runs one
set and a readback at a time, and presses meanwhile accumulate (steps
sum, mute toggles fold to their parity).

- A key moves `volume_step` percent (5). Raising stops at `volume_max`
  (100 %), but a level already past it (set elsewhere) is never pulled
  down; the capsule shows the true percent over a full bar.
- **Headset buttons**: a Bluetooth headset with AVRCP absolute volume
  presses no key. BlueZ announces the change on the system bus; the sink
  is read back 250 ms after the last announcement (PipeWire waits
  200 ms), and the capsule shows only a level or mute that moved. A
  headset without absolute volume sends volume keys to your binds.

## Touchpad

Off while an external (USB or Bluetooth) mouse is present, on again when
it goes.

- A pointer counts as a mouse once it moves or clicks, so a keyboard-only
  receiver, a gaming keyboard's mouse interface or a presenter never
  does. With an external trackpad too, the policy drives the built-in
  one.
- `touchpad_toggle` (`XF86TouchpadToggle`) flips it by hand until a
  pointer device is plugged or unplugged (virtual pointers don't count).
- The flip writes the compositor's per-device config (`hl.device`). A
  config reload wipes that and the plugin re-applies it silently: a
  capsule answers only a visible change or the manual flip.

## Feedback

An action that lands answers with a capsule at the top center: a glyph
with a level track and number, or words; a muted volume or microphone
reads "Muted" or "Off" in the urgent colour. A refused write or an
unparsable readback shows nothing rather than a wrong value. Capsules
replace in place (one per control), last 1.2 s (touchpad 1.5 s), and are
feedback, not notifications: no center row, no dot, and opening the
center drops a live one. The battery's critical alert shares the id band
but is a real alert that waits in the center.
