# hyprland-plugins

One native C++26 Hyprland plugin for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork
ABI: [`awesome`](awesome/), the awesome shell as a monolith.

| module | owns |
|---|---|
| `core` | the platform: supervisor, input pipeline, hop queue, canvas (warm/draw gate), config schema, stores, jobs, fd.o bus client |
| `shell` | the bar: workspaces, tasks (views), tray, bell, battery, clock, menubar launcher |
| `windows` | the window state machine and all placement: maximize/minimize/restore, click/focus policy, spawn placement, drag snap |
| `notify` | the `org.freedesktop.Notifications` daemon: cards, conversations, shade, popups, inline reply, sound |
| `system` | volume/mic (wpctl), brightness (logind), touchpad policy — every action's feedback is a notify card |

The behavior docs live in [`awesome/README.md`](awesome/README.md);
the design contract in
[`docs/awesome-design.md`](docs/awesome-design.md).

## Install

```sh
hyprpm add https://github.com/hitori-chan/hyprland-plugins
hyprpm enable awesome
```

The plugin owns `org.freedesktop.Notifications`; disable another daemon
such as dunst or mako before enabling it. Point `hyprpm` at the matching
fork before an update:

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
```

## Development

```sh
make              # the plugin
make test         # headless core harness
make gate ARGS=...  # the nested integration gate (see awesome/gate/)
```

Before deployment, run the nested gate described in
[`awesome/gate/gate.sh`](awesome/gate/gate.sh) and require its final
`ALL CHECKS PASSED` line. The gate drives the shared input fixtures in
[`devtools/`](devtools/README.md). Never rebuild or replace the plugin
while a compositor has it mapped.
