# awesome

An AwesomeWM-flavoured desktop shell for Hyprland, as one native C++26
plugin:

- a bar and a launcher;
- window policy;
- a notification daemon;
- the volume, brightness and touchpad keys.

## Enable

```sh
hyprpm enable awesome
```

`awesome` is the session's notification daemon. Stop dunst, mako or any
other daemon before enabling it.

## Use

```lua
hl.bind("SUPER + P", function() hl.plugin.awesome.menubar() end)
hl.bind("F12", function() hl.plugin.awesome.center() end)
hl.bind("XF86AudioRaiseVolume", function() hl.plugin.awesome.volume_up() end)

hl.config({ plugin = { awesome = { shell = { terminal = "foot" } } } })
```

The bar reserves its own space, so a monitor rule doesn't need
`reserved_area`. The battery only shows on machines that have one.

## Docs

- [Shell](docs/shell.md): the bar and the launcher
- [Windows](docs/windows.md): minimize, maximize, focus, placement, snap
- [Notifications](docs/notify.md): cards, conversations, the center, Do
  Not Disturb
- [System](docs/system.md): brightness, sound, touchpad
- [Reference](docs/reference.md): every Lua function, `hyprctl` command,
  config key and remembered state
- [Design](docs/design.md): architecture, invariants, performance
- [Testing](docs/testing.md): building, unit tests, the nested gate
