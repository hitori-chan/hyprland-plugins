# vclick

Clicks, scrolls, hovers, drags or pinches inside a window without the
user's mouse, and sends it keys without the user's keyboard. By default it
glides there first, along a curve at the user's own pace. Shares no code
or state with `awesome`.

```sh
hyprpm enable vclick
```

## Use

```sh
W=$(hyprctl -j clients | jq -r '.[] | select(.class == "firefox") | .address' | head -1)
hyprctl vclick click  $W 640 300                 # glide there, click
hyprctl vclick click  $W 640 300 double          # double-click
hyprctl vclick click  $W 640 300 right           # a context menu
hyprctl vclick scroll $W 640 500 down 12         # 12 wheel steps at the user's pace
hyprctl vclick scroll $W 640 500 up 3 200ms      # 200 ms apart
hyprctl vclick scroll $W 640 500 down 400px      # a touchpad finger's, by pixels
hyprctl vclick move   $W 640 300                 # hover there
hyprctl vclick press  $W 100 400                 # hold the button,
hyprctl vclick move   $W 500 400                 #   drag it,
hyprctl vclick release                           #   let go
hyprctl vclick drag   $W 100 400 500 400         # the same in one request
hyprctl vclick pinch  $W 640 300 2               # two fingers zoom in twice
hyprctl vclick click  $W 640 300 performance     # at once, no glide
hyprctl vclick key    $W Next                    # PageDown, to that window only
hyprctl vclick key    $W t ctrl+shift            # with modifiers
hyprctl vclick where                             # address:0x… 640 300
hyprctl vclick hide                              # lift the mouse
hyprctl vclick status                            # moving, scrolling 3/12, idle
```

The same verbs are Lua functions (`hyprctl repl`):
`hl.plugin.vclick.click(W, 640, 300, "performance")` returns `true`, or
`nil` and the error.

The window is any selector (`address:0x…`, `class:…`) or a bare `0x…`
address; coordinates are a window capture's pixels (`grim -T`) from its
top-left. The user's cursor, keyboard focus and the window under their
mouse are untouched.

Two modes (`hyprctl vclick mode human|performance`); a word on a request
picks its timing:

- `human` (default): a mouse of its own. It glides there and stays: the
  app keeps the hover, and vclick draws the app's cursor where it rests
  (under windows above, never in a capture). The user's pointer coming
  into that app takes it over.
- `performance`: just the input, as fast as apps still read it right;
  the app's pointer leaves after, nothing stays drawn.

Under the lockscreen requests still reach the named window, and only it;
the user's input goes only to the lockscreen, and no cursor is drawn.

## The user's pace

`learn.py` records the user's mouse and fits the profile the glide moves
with (time by distance, curve, speed profile, drift, slow ends,
corrections, rest, button and wheel timing):

```sh
python3 -I vclick/learn.py record --minutes 30   # use the mouse as usual meanwhile
python3 -I vclick/learn.py fit                   # writes ~/.local/state/vclick/profile
hyprctl vclick profile reload
```

`record` needs the `input` group; it reads pointer devices only and grabs
none. Without a profile, a typical mouse user's pace.

## Docs

- [Reference](docs/reference.md): every verb and answer, Lua, the
  profile file, the errors.
- [Design](docs/design.md): how it works and its limits.
- [Testing](docs/testing.md): build, unit cases, the nested check.
