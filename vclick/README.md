# vclick

A plugin that clicks, scrolls or hovers inside a window without the
user's mouse. By default it glides there first, along a curve and at the
user's own pace. It shares no code or state with `awesome`.

## Enable

```sh
hyprpm enable vclick
```

## Use

```sh
W=$(hyprctl -j clients | jq -r '.[] | select(.class == "firefox") | .address' | head -1)
hyprctl vclick address:$W 640 300                  # glide there, click a link
hyprctl vclick address:$W 640 300 left 2           # double-click a word
hyprctl vclick address:$W 640 500 scroll-down 12   # 12 wheel steps, 50 ms apart
hyprctl vclick address:$W 640 300 move             # hover there, and stay
hyprctl vclick address:$W 640 300 jump             # click at once, no glide
hyprctl vclick status                              # moving, scrolling 3/12, then idle
```

The coordinates are pixels of a window capture (`grim -T`), measured from
the window's top-left corner. The user's cursor doesn't move, the window
under the mouse hears nothing, and keyboard focus stays where it is.
While an input runs, vclick draws the target app's own cursor image over
the window at its pointer (never in a capture), then fades it out.

## The user's pace

`learn.py` records the user's own mouse for a while and fits the profile
the glide moves with: how long a move takes for its distance (Fitts's
law), how much it curves, where it is fastest, how often it overshoots,
and how long the pointer rests before a click.

```sh
python3 -I vclick/learn.py record --minutes 30   # use the mouse as usual meanwhile
python3 -I vclick/learn.py fit                   # writes ~/.local/state/vclick/profile
hyprctl vclick profile reload
```

`record` needs the `input` group. It reads only pointer devices (motion
and mouse buttons, never a keyboard) and grabs none. Without a profile
the glide moves at a typical mouse user's pace.

## Docs

- [Reference](docs/reference.md): `hyprctl vclick`, `status`, `mode`,
  `profile`, `cursor`, the profile file, the errors
- [Design](docs/design.md): input without the seat, the glide, the
  cursor image, the hover, paced scrolling, the same app, limits
- [Testing](docs/testing.md): the build, the path's unit cases, the
  nested check
