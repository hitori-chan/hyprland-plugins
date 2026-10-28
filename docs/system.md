# System

The `system` module: the display and keyboard lights, volume and
microphone, Bluetooth, and the touchpad. Its levels and devices show in
the [control center](notify.md#the-control-center); its keys answer with
a capsule.

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
- The output pill in the control center's Sound row lists the outputs
  and makes one the default.
- **A headset's own buttons**: a Bluetooth headset that sets its volume
  itself (AVRCP absolute volume) presses no key. BlueZ announces the
  change on the system bus, the default sink is read back, and the
  capsule shows only a level or a mute that actually moved. A headset
  without absolute volume sends volume keys, which reach your binds.

## Bluetooth

BlueZ's first adapter and its paired devices, over the system bus and
never waiting on it: the tree is read when the plugin loads, when
bluetoothd (re)starts and when the control center opens, and BlueZ's
own signals keep it current in between. Without an adapter the control
center shows no Bluetooth.

- **Devices** are the paired ones (a device only seen in a scan is not
  listed): connected first, then by name, at most 16, a name cut at 64
  bytes. A device's battery shows where BlueZ reports one.
- **Connecting** (and **Disconnecting**) shows until bluetoothd answers
  (a headset bringing up its profiles takes seconds; the call waits up to
  30 s). A device picked while the radio is off powers it on first, then
  connects.
- **The kill switch** is the kernel's: `/dev/rfkill`, read as its
  events come. A hard block (a switch, the firmware) reads Blocked; a
  soft one the circle lifts (the seat's user may write rfkill, logind's
  uaccess), and the adapter powers on once BlueZ's PowerState leaves
  off-blocked. Without a writable rfkill a blocked radio's power-on is
  "Didn't Turn On".
- **A refusal says why.** BlueZ's answer becomes a reason: a page
  timeout, "Host is down" or no answer in 30 s is "Didn't Answer"; a
  missing key or a failed authentication "Pairing Lost"; anything else
  "Didn't Connect" (another connect already running is no refusal). While
  the control center shows, the device's row and Bluetooth's capsule say
  it until the panel closes; with the panel closed, a red capsule at the
  top says it for 3 s ("WH-1000XM4 Didn't Answer"). A refused power-on
  is "Didn't Turn On".
- **Forget** removes the device from the adapter (`RemoveDevice`), its
  keys with it; BlueZ's InterfacesRemoved takes it off the list.
- **Scan** is the menu's: `StartDiscovery` on Scan, `StopDiscovery` on
  Stop, a pick, after a minute or when the menu closes. A device lists
  once it is unpaired, has a name and was seen by the scan, ranked by the
  signal it was first seen at (a later reading moves nothing: the rows
  hold still under the pointer); its list stays until the menu closes.
  Unpaired devices are held only while the menu's scan session lasts, at
  most 32.
- **Pairing** goes through the plugin's own agent
  (`/org/awesome/bluetooth/agent`, capability DisplayYesNo: the menu shows
  a code and has no field to type one), registered at the first pairing
  and never made the default. BlueZ asks the agent of the client that
  called `Pair`, so another app's pairing never reaches it; it answers
  only bluetoothd, about the device being paired, and rejects the rest
  (an incoming pairing, a service authorization). A legacy device's PIN
  is ours, random, shown to be typed on it. A paired device is set
  Trusted, then connected.
- **A headset's sink**: when an audio device connects or drops while the
  control center is open, the outputs are read again once PipeWire has
  caught up (1.5 s), so the output pill follows.

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
notifications: no center row and no dot.

While the control center is open its sliders show a level's change
instead, and no capsule covers them; opening it drops a live level
capsule. The microphone's and the touchpad's keys keep their capsules
there (the panel has no control for either). The
battery's critical alert shares the capsules' id band but is a real
alert: it waits in the center like any banner.
