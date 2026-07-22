# hyprland-plugins

Native Hyprland plugins, AwesomeWM-inspired, one behavior each — built for
native Hyprland/Wayland design, performance and efficiency.
Built and loaded with [`hyprpm`](https://wiki.hypr.land/Plugins/Using-Plugins/).

| plugin       | what it does |
|--------------|--------------|
| `hyprbar`    | the awesome wibar: kanji taglist, per-workspace tasklist, tray with menus, menubar launcher, battery, clock |
| `hyprnotify` | awesome's `naughty`: the compositor is the `org.freedesktop.Notifications` daemon — cards with kickers and image icons, waifu fallback, hero previews, DND, click-to-raise |
| `hyprmax`    | awesome's per-window maximize: any number at once, immovable while maximized, windowed size remembered per app |
| `hyprclick`  | awesome's click/focus policy: click-to-raise, keyboard focus raises, hover never does |
| `hyprsnap`   | awesome's `awful.mouse.snap`: magnetic edge pull + aerosnap halves/quarters while dragging |
| `hyprplace`  | spawn placement: last spot per app, else centered, else the largest gap |
| `hyprpad`    | the awesome touchpad module: touchpad off while an external mouse is present, XF86TouchpadToggle by hand |
| `hyprosd`    | the awesome volume/brightness OSD: XF86 keys → value-bar cards; sysfs+logind brightness, wpctl volume off the hot paths |

## Install

```sh
hyprpm add https://github.com/hitori-chan/hyprland-plugins
for p in hyprbar hyprnotify hyprmax hyprclick hyprsnap hyprplace hyprpad hyprosd; do hyprpm enable $p; done
```

Load order = `hyprpm.toml` order, and it matters: `hyprbar` first (it
swallows bar clicks before `hyprclick` sees them), `hyprnotify` before
`hyprmax`/`hyprclick` (a card click never reaches the window beneath),
`hyprmax` before `hyprclick` (the immovable-maximized swallow wins over
click-to-raise). Load at login with `hyprpm reload -n` in the autostart.
`hyprnotify` replaces any external daemon (dunst, mako): nothing else may
own `org.freedesktop.Notifications`.

## Building against a Hyprland fork

hyprpm compiles plugins against the running compositor's headers. For a
fork, point it at the fork's repo — the running commit must be pushed:

```sh
hyprpm update --hl-url https://github.com/hitori-chan/Hyprland
```

## Layout

One directory per plugin, each Makefile builds its `.so` in place through
the shared build in [`common/common.mk`](common/common.mk);
[`hyprpm.toml`](hyprpm.toml) is the manifest. `common/` holds the shared
machinery (lifecycle, queries, persistence, bus link, texture cache,
load-order asserts); `devtools/` holds the test tooling. `hyprbar`
additionally links `sdbus-c++`, `librsvg` and `libudev`; `hyprnotify` links
`sdbus-c++` and `hyprgraphics`; `hyprpad` links `sdbus-c++` and `libinput`;
`hyprosd` links `sdbus-c++`.
