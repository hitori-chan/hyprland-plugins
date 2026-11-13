# vclick

A plugin that clicks, scrolls or hovers inside a window without the
user's mouse, and sends it keys without the user's keyboard. By default
it glides there first, along a curve and at the user's own pace. It
shares no code or state with `awesome`.

## Enable

```sh
hyprpm enable vclick
```

## Use

```sh
W=$(hyprctl -j clients | jq -r '.[] | select(.class == "firefox") | .address' | head -1)
hyprctl vclick click  $W 640 300                 # glide there, click a link
hyprctl vclick click  $W 640 300 double          # double-click a word
hyprctl vclick click  $W 640 300 right           # a context menu
hyprctl vclick scroll $W 640 500 down 12         # 12 wheel steps, 50 ms apart
hyprctl vclick scroll $W 640 500 up 3 200ms      # slower
hyprctl vclick move   $W 640 300                 # hover there
hyprctl vclick press  $W 100 400                 # hold the button down there,
hyprctl vclick move   $W 500 400                 #   drag it (a slider, a selection),
hyprctl vclick release                           #   and let go
hyprctl vclick click  $W 640 300 jump            # click at once, no glide
hyprctl vclick key    $W Next                    # PageDown, to that window only
hyprctl vclick key    $W t ctrl+shift            # with modifiers held for it
hyprctl vclick where                             # address:0x… 640 300
hyprctl vclick hide                              # take the mouse away
hyprctl vclick status                            # moving, scrolling 3/12, then idle
```

The same verbs are Lua functions, for a bind or a script
(`hyprctl repl`): `hl.plugin.vclick.click(W, 640, 300, "jump")` answers
`true`, or `nil` and the error.

The window is any window selector (`address:0x…`, `class:…`) or a bare
`0x…` address. The coordinates are pixels of a window capture
(`grim -T`), measured from the window's top-left corner. The user's
cursor doesn't move, the window under the mouse hears nothing, and
keyboard focus stays where it is. A key goes to that one window, pressed
and released, and the window with the user's keyboard hears nothing.

Two modes (`hyprctl vclick mode mouse|tap`):

- `mouse` (the default) is a mouse of its own: it glides there, and
  stays. The app keeps the hover, and vclick draws the app's own cursor
  (its image, or its shape from the cursor theme) in the window where it
  rests: under any window above it, never in a capture. The user's own pointer coming into that app takes it, as
  one mouse: the cursor goes, and the next input starts where their
  pointer left the window.
- `tap` is just the input: it goes at once, and leaves the app's pointer
  after. Nothing stays drawn.

## Under the lockscreen

A script keeps going while the session is locked (reading a window
while the user is away): clicks, scrolls, moves and keys reach the
window they name, and only it. The user's own keyboard and mouse still
go only to the lockscreen, the lockscreen keeps its focus, and no cursor
is drawn.

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

- [Reference](docs/reference.md): `click`, `scroll`, `move`, `press`,
  `release`, `key`, `status`, `where`, `hide`, `mode`, `profile`,
  `image`, the Lua functions, the profile file, the errors
- [Design](docs/design.md): input without the seat, the modes, the
  glide, the cursor image, the hover, a held button, paced scrolling,
  the same app, the keyboard, under the lockscreen, limits
- [Testing](docs/testing.md): the build, the unit cases, the nested
  check
