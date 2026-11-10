# vclick

A single-file plugin that clicks or scrolls inside a window without the
user's mouse. It shares no code or state with `awesome`.

## Enable

```sh
hyprpm enable vclick
```

## Use

```sh
W=$(hyprctl -j clients | jq -r '.[] | select(.class == "firefox") | .address' | head -1)
hyprctl vclick address:$W 640 300                  # click a link
hyprctl vclick address:$W 640 300 left 2           # double-click a word
hyprctl vclick address:$W 640 500 scroll-down 12   # 12 wheel steps, 50 ms apart
hyprctl vclick status                              # scrolling 3/12, then idle
```

The coordinates are pixels of a window capture (`grim -T`), measured from
the window's top-left corner. The cursor doesn't move, its image doesn't
change, the window under the mouse hears nothing, and keyboard focus stays
where it is.

## Docs

- [Reference](docs/reference.md): `hyprctl vclick`, `status`, the errors
- [Design](docs/design.md): input without the seat, paced scrolling, the
  same app, limits
- [Testing](docs/testing.md): the build, the nested check
