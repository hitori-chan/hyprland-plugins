# hyprland-plugins

Native C++26 Hyprland plugins inspired by AwesomeWM and built for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork ABI.

| Plugin | Purpose |
|---|---|
| `hyprbar` | Compositor-drawn wibar: tags, tasks, tray, launcher, notifications, battery, clock, layout |
| `hyprnotify` | Freedesktop notification daemon with Android-style banners, center, DND, policy, and replies |
| `hyprmax` | Per-window maximize with remembered windowed geometry |
| `hyprsnap` | Magnetic edge snapping and aerosnap halves/quarters |
| `hyprclick` | Click-to-raise; focused windows raise |
| `hyprplace` | Per-app spawn memory with least-overlap fallback |
| `hyprpad` | Touchpad auto-disable and manual toggle policy |
| `hyprosd` | Asynchronous volume and brightness OSD feedback through `hyprnotify` |

## Install

```sh
hyprpm add https://github.com/hitori-chan/hyprland-plugins
for p in hyprbar hyprnotify hyprmax hyprsnap hyprclick hyprplace hyprpad hyprosd; do hyprpm enable "$p"; done
```

Load order follows [`hyprpm.toml`](hyprpm.toml) and is behavioral: bar and
notification input must win before window policy, and maximize must win before
click-to-raise. `hyprnotify` owns `org.freedesktop.Notifications`, so disable
other notification daemons such as dunst or mako.

## Fork Build

`hyprpm` must compile against the headers of the compositor that will load the
plugins. Point it at the pushed fork before updating:

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
```

## Repository

- One directory and README per plugin.
- [`common/`](common/) owns shared lifecycle, input, persistence, bus, process,
  icon, theme, and texture helpers.
- [`devtools/`](devtools/) contains standalone regressions and the required
  nested-compositor gate.
- [`docs/hyprbar.md`](docs/hyprbar.md) and
  [`docs/hyprnotify.md`](docs/hyprnotify.md) document implementation contracts
  that do not belong in user-facing plugin READMEs.
