# hyprland-plugins

`awesome`: an AwesomeWM-flavoured desktop shell for Hyprland as one
native C++26 plugin — a bar and launcher, window policy, a notification
daemon, and the volume, brightness and touchpad keys.

`vclick`: a separate, single-file plugin that clicks or scrolls inside a
window without the user's mouse — the cursor, the hover and the focus
stay the user's ([vclick](vclick/docs/reference.md)).

Both are built against the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork,
not upstream Hyprland.

## Install

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
hyprpm add https://github.com/hitori-chan/hyprland-plugins
hyprpm enable awesome
hyprpm enable vclick    # optional, independent of awesome
```

`awesome` is the session's notification daemon: stop dunst, mako or
any other before enabling it.

## Use

```lua
hl.bind("SUPER + P", function() hl.plugin.awesome.menubar() end)
hl.bind("F12", function() hl.plugin.awesome.center() end)
hl.bind("XF86AudioRaiseVolume", function() hl.plugin.awesome.volume_up() end)

hl.config({ plugin = { awesome = { shell = { terminal = "foot" } } } })
```

The bar reserves its own band: a monitor rule needs no `reserved_area`.
The battery shows only where the machine has one.

Every function, `hyprctl` verb, config key and remembered state is in
the [reference](awesome/docs/reference.md).

## Docs

`awesome`:

- [Shell](awesome/docs/shell.md) — the bar and the launcher
- [Windows](awesome/docs/windows.md) — minimize, maximize, focus, placement, snap
- [Notifications](awesome/docs/notify.md) — cards, conversations, the center,
  Do Not Disturb
- [System](awesome/docs/system.md) — brightness, sound, touchpad
- [Reference](awesome/docs/reference.md) — Lua API, `hyprctl`, configuration, state
- [Design](awesome/docs/design.md) — architecture, invariants, performance
- [Testing](awesome/docs/testing.md) — building, unit tests, the nested gate

`vclick`:

- [Reference](vclick/docs/reference.md) — `hyprctl vclick`, its errors
- [Design](vclick/docs/design.md) — input without the seat, the same app, limits
- [Testing](vclick/docs/testing.md) — the build, the nested check

## Development

```sh
make          # the plugins
make test     # unit tests
make gate ARGS="-s ~/repo/Hyprland -b all"   # the nested integration gate
make vclick-check                            # vclick's nested check
```

Contributors and coding agents start at [`AGENTS.md`](AGENTS.md).
