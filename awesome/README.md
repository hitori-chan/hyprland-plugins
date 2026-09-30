# awesome

One native C++26 Hyprland plugin for the exact
[`hitori-chan/Hyprland`](https://github.com/hitori-chan/Hyprland) fork
ABI. Five modules, one `PLUGIN_INIT`, one input pipeline, no
inter-module bus:

| module | owns |
|---|---|
| `core` | the platform: supervisor, input pipeline, hop queue, canvas (warm/draw gate), config schema, stores, jobs, fd.o bus client |
| `shell` | the bar: workspaces, tasks (views), tray, bell, battery, clock, menubar launcher |
| `windows` | the window state machine and all placement: maximize/minimize/restore, click/focus policy, spawn placement, drag snap |
| `notify` | the `org.freedesktop.Notifications` daemon: cards, conversations, shade, popups, inline reply, sound |
| `system` | volume/mic (wpctl), brightness (logind), touchpad policy — every action's feedback is a notify card |

The plugin owns `org.freedesktop.Notifications`; disable another
daemon (dunst, mako) before enabling it.

## Lua API

```lua
hl.plugin.awesome.shell.menubar()          -- the launcher
hl.plugin.awesome.windows.maximize()       -- focused window
hl.plugin.awesome.windows.minimize()
hl.plugin.awesome.windows.restore()
hl.plugin.awesome.windows.focus_next()
hl.plugin.awesome.windows.focus_prev()
hl.plugin.awesome.windows.focus_prev_here()
hl.plugin.awesome.notify.center()          -- toggles the shade
hl.plugin.awesome.notify.suspend()         -- toggles DND
hl.plugin.awesome.notify.clear_all()
hl.plugin.awesome.system.volume_up()       -- volume_down, mute, mic_mute
hl.plugin.awesome.system.brightness_up()   -- brightness_down
hl.plugin.awesome.system.touchpad_toggle()
```

The bell also peeks the shade on pointer hover (400 ms grace; moving
off cancels, a click pins).

## hyprctl

`hyprctl awesome <verb>` routes to the owning module: notify serves
`count`, `center`, `state`, `badge`, `topline`, `clear`; system serves
`pad`.

## Configuration

48 keys under `plugin:awesome:<module>:<key>` (shell 21, notify 24,
windows 3). The table — kinds, defaults, clamps — is
[`core/schema.cpp`](core/schema.cpp); the defaults are the glass·ink
theme tokens, so an empty config still looks right.

State lives in `$XDG_STATE_HOME/awesome/` (`windows-spot.tsv`,
`windows-windowed.tsv`, `shell-launches.tsv`, `shell-history.tsv`);
first run migrates the old per-plugin stores once and never touches
them again.

## Build and test

```sh
make -C awesome            # the plugin (needs the fork headers)
make -C awesome test       # headless core harness, no fork headers
make -C awesome gate       # the nested integration gate (gate/gate.sh;
                           # ARGS passes through: bin, -b/-k battery selection)
```

Before a release, run the gate and require its final `ALL CHECKS
PASSED` line. The gate is the plugin's behavioral contract: six
scenario batteries (shell/windows/notify/system/pipeline/lifecycle)
against a throwaway nested compositor, with the old->new check map in
gate/manifest.tsv. The input fixtures it drives (vptr, vkbd, cliphold,
…) are the shared devtools/.
