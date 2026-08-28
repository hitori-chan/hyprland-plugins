# hyprosd

Asynchronous volume, microphone, and brightness controls with feedback rendered
by `hyprnotify`. No config.

- Brightness reads `/sys/class/backlight`, applies 5% linear steps with a raw
  floor of 2, and writes through logind `Session.SetBrightness`.
- Volume and microphone changes use bounded `wpctl` set/readback chains.
  PipeWire process I/O stays off render and input callbacks; readback rejects
  malformed or non-finite output.
- Fixed notification IDs replace feedback in place. Cards are low urgency,
  expire after 1200 ms, carry a value bar, and select native brightness,
  volume, microphone, or mute identities.
- Shutdown invalidates process callbacks and closes descriptors without waiting
  for a stuck child.

Bind these functions through `hl.plugin.hyprosd`: `volume_up`, `volume_down`,
`mute`, `mic_mute`, `brightness_up`, and `brightness_down`.

```lua
hl.bind({ mods = "", key = "XF86AudioRaiseVolume",
          press = function() local p = hl.plugin.hyprosd; if p then p.volume_up() end end })
```
