# hyprland-plugins

Native Hyprland plugins reproducing the AwesomeWM feel, one behavior each.
Built and loaded with [`hyprpm`](https://wiki.hypr.land/Plugins/Using-Plugins/).

| plugin      | what it does |
|-------------|--------------|
| `hyprbar`   | the awesome wibar: kanji taglist, per-workspace tasklist, tray with menus, menubar launcher, battery, clock |
| `hyprmax`   | awesome's per-window maximize: any number at once, immovable while maximized, windowed size remembered per app |
| `hyprclick` | awesome's click/focus policy: click-to-raise, keyboard focus raises, hover never does |
| `hyprsnap`  | awesome's `awful.mouse.snap`: magnetic edge pull + aerosnap halves/quarters while dragging |
| `hyprplace` | spawn placement: last spot per app, else centered, else the largest gap |

## Install

```sh
hyprpm add https://github.com/hitori-chan/hyprland-plugins
for p in hyprbar hyprmax hyprclick hyprsnap hyprplace; do hyprpm enable $p; done
```

Load order = `hyprpm.toml` order, and it matters twice: `hyprbar` first (it
swallows bar clicks before `hyprclick` sees them), `hyprmax` before
`hyprclick` (the immovable-maximized swallow wins over click-to-raise).
Load at login with `hyprpm reload -n` in the autostart.

## Building against a Hyprland fork

hyprpm compiles plugins against the running compositor's headers. For a
fork, point it at the fork's repo — the running commit must be pushed:

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
```

## Layout

One directory per plugin, each Makefile builds its `.so` in place;
[`hyprpm.toml`](hyprpm.toml) is the manifest. `hyprbar` additionally links
`sdbus-c++` and `librsvg`.
