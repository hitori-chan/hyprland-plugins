# hyprland-plugins

`awesome`: an AwesomeWM-flavoured desktop shell for Hyprland as one
native C++26 plugin — a bar and launcher, window policy, a notification
daemon with a control center, and the system controls.

It is built against the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork,
not upstream Hyprland.

## Install

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
hyprpm add https://github.com/hitori-chan/hyprland-plugins
hyprpm enable awesome
```

The plugin is the session's notification daemon: stop dunst, mako or
any other before enabling it.

## Use

```lua
hl.bind("SUPER + P", function() hl.plugin.awesome.menubar() end)
hl.bind("F12", function() hl.plugin.awesome.center() end)
hl.bind("XF86AudioRaiseVolume", function() hl.plugin.awesome.volume_up() end)

hl.config({ plugin = { awesome = { shell = { terminal = "foot" } } } })
```

Every function, `hyprctl` verb, config key and remembered state is in
the [reference](docs/reference.md).

## Docs

- [Shell](docs/shell.md) — the bar and the launcher
- [Windows](docs/windows.md) — minimize, maximize, focus, placement, snap
- [Notifications](docs/notify.md) — cards, conversations, the center
- [System](docs/system.md) — lights, sound, touchpad
- [Reference](docs/reference.md) — Lua API, `hyprctl`, configuration, state
- [Design](docs/design.md) — architecture, invariants, performance
- [Testing](docs/testing.md) — building, unit tests, the nested gate

## Development

```sh
make          # the plugin
make test     # unit tests
make gate ARGS="-s ~/repo/Hyprland -b all"   # the nested integration gate
```

Contributors and coding agents start at [`AGENTS.md`](AGENTS.md).
