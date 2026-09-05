# hyprland-plugins

Eight native C++26 plugins for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork ABI.

| Plugin | Responsibility |
|---|---|
| `hyprbar` | Top bar, workspaces, tasks, tray, launcher, battery, clock, and notification bell |
| `hyprnotify` | Freedesktop notification daemon, banners, notification center, DND, policy, and replies |
| `hyprmax` | Per-window maximize with remembered windowed geometry |
| `hyprsnap` | Magnetic and edge/corner snapping |
| `hyprclick` | Click-to-raise and stable focus cycling |
| `hyprplace` | Remembered floating-window placement with least-overlap fallback |
| `hyprpad` | Automatic and manual touchpad policy |
| `hyprosd` | Asynchronous volume, microphone, and brightness feedback through `hyprnotify` |

Load order is part of the input contract and is fixed in
[`hyprpm.toml`](hyprpm.toml):

```text
hyprbar -> hyprnotify -> hyprmax -> hyprsnap -> hyprclick -> hyprplace -> hyprpad -> hyprosd
```

## Install

```sh
hyprpm add https://github.com/hitori-chan/hyprland-plugins
for plugin in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do
    hyprpm enable "$plugin"
done
```

`hyprnotify` owns `org.freedesktop.Notifications`; disable another daemon such
as dunst or mako before using it. Point `hyprpm` at the matching fork before an
update:

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
```

## Development

Each plugin has its own user-facing README. Shared lifecycle, bus, persistence,
icon, theme, process, and texture helpers live in [`common/`](common/).
Implementation contracts that cross files live in
[`docs/hyprbar.md`](docs/hyprbar.md) and
[`docs/hyprnotify.md`](docs/hyprnotify.md).

Build everything from the repo root, or one plugin and the standalone
regressions:

```sh
make            # all plugins, sequential
make test       # devtools unit tests
make -C hyprnotify
make -C devtools test
```

Before deployment, run the exact-fork nested gate described in
[`devtools/README.md`](devtools/README.md) and require its final
`ALL CHECKS PASSED` line. Never rebuild or replace a plugin while a compositor
has that plugin mapped.
