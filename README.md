# hyprland-plugins

Hyprland plugins, built against the
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork
rather than upstream Hyprland.

| Plugin | What it does |
|---|---|
| [awesome](awesome/README.md) | an AwesomeWM-flavoured shell: bar, launcher, window policy, notifications, system keys |
| [vclick](vclick/README.md) | click or scroll inside a window without the user's mouse |

## Install

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
hyprpm add https://github.com/hitori-chan/hyprland-plugins
hyprpm enable awesome
hyprpm enable vclick    # optional, independent of awesome
```

## Development

```sh
make                # both plugins
make test           # awesome's unit tests
make gate ARGS=…    # awesome's nested gate
make vclick-check   # vclick's nested check
```

Contributors and coding agents start at [`AGENTS.md`](AGENTS.md).
