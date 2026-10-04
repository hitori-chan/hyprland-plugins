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

## Behavior

### The bar (shell)

The strip, launcher, and tray menus are compositor-drawn on the focused
monitor. Before intercepting any input the pipeline rechecks session
lock and native input ownership (layers, popups, IME, grabs,
input-capture); native wins and resets the plugin's partial input
state. Geometry and hitboxes are monitor-local; fixed widget slots and
tray clipping protect the task area on narrow outputs. Textures follow
the canvas warm/draw gate: warm on the event loop, draw no earlier than
the next frame, scissor to damage, damage both old and new geometry for
visible transitions.

- **Workspaces** render as tag cells, **tasks** as view cells, both
  pixel-drawn at their own measured widths (nothing fixed-pitch).
- **Tasks**: Hyprland has no plugin-owned minimized state, so the
  windows module removes a minimized window from layout/render
  participation and remembers its fullscreen, maximize, and floating
  state for restoration. Tasks stay in arrival order; focus cycling
  skips minimized windows. A task's minimize/restore repaints through
  the task-changed hook.
- **Tray**: owns the StatusNotifier/dbusmenu connection. A tray click
  is the activation — after the SNI `Activate` call the bar resolves
  the item's bus PID and focuses the app's own topmost window, for
  every backend (core/activate.hpp). The plugin performs the raise
  itself because no backend can be trusted to: an X11 client's
  SetForegroundWindow arrives as an urgency ping (unauthenticated — the
  fork maps it to urgency only), and a Wayland sender may simply never
  spend the token; in vanilla awesome the app activates itself after
  the click, and the plugin makes that unconditional. A sender that DID
  spend the token just lands on its already-focused window.
  Menus support updates, cascades, scrolling, and check/radio state;
  providers may emit a leading, doubled, or trailing separator and the
  parse trims them. Keyboard navigation, tooltips, and overlay icons
  are not implemented.
- **Bell**: reads the notify model directly (same process — no bus
  seam). A click toggles the shade; pointer hover does nothing.
- **Battery** watches the udev battery; the clock ticks per second.

### The launcher (menubar)

Desktop discovery and completion run in a bounded cancellable helper
process. The compositor drains framed results through the event loop;
queue backpressure pauses the helper, and teardown closes the pipe and
terminates its private process group without joining filesystem work.
Desktop Entry strings are decoded before use; `Exec=` preserves its
quoting and field-code grammar before passing argument boundaries to
the executor.

| Keys | Action |
|---|---|
| `Left/Right`, `C-j/k`, `Home/End` | select |
| `Enter` | run selected entry |
| `C-Return`, `C-M-Return` | run raw query directly or in a terminal |
| `Tab`, `Shift-Tab` | complete command/path |
| `Up/Down`, `C-p/n` | history |
| `C-a/e/b/f/d/h/u/w`, `M-b/f/d`, `C-BackSpace` | edit |
| `C-v` | paste asynchronously |
| `Escape` or pointer click | close |

Typed and pasted input shares a 4 KiB UTF-8 limit. Clipboard transfer
is nonblocking with a 1.5-second deadline; closing or tearing down the
launcher removes the fd and invalidates the callback. History lives in
the plugin's one state file (see State).

### Windows

- **Maximize** is per-window and does not consume Hyprland's
  fullscreen slot: the window fills the current workarea and carries
  the native xdg maximized state; app- or compositor-maximized windows
  are adopted into the same model, and a client's own unmaximize (its
  CSD titlebar restore button or double-click) leaves it the same way
  `Mod+M` does — the compositor alone drops that request for a window it
  does not hold maximized, which left such clients told maximized and
  reopening maximized. Maximized windows follow workspace, output, and
  reserved-area changes, and the pointer follows every such geometry
  change (a window that grows under a still cursor takes the next click). `Mod+click` drags are swallowed
  while maximized so the raise policy never fights the drag. Windowed
  geometry is restored from the store, constrained by the current
  workarea and the client's size hints.
- **Click policy**: a plain left click raises the target; clicking a
  maximized window tucks fullscreen-flagged floaters without rewriting
  z-order. Keyboard focus raises, pointer hover does not. A short-lived
  corpse guard consumes presses aimed at a window that just closed or
  left fullscreen, preventing click-through to the window below.
  `focus_next`/`focus_prev` cycle stable arrival order (not the
  z-order that click-to-raise keeps changing); `focus_prev_here`
  toggles the two most recent windows on the current workspace.
- **Focus semantics (attention-first, the shipped default)**: the
  mechanism is awesome's `permissions.activate` (source-verified), and
  the shipped default picks the Windows foreground model — a background
  app's "present me" asks for ATTENTION, never the focus (this is what
  keeps a message arriving in Telegram from stealing the keyboard):
  - A newly mapped window takes initial focus (awesome's global
    `focus = awful.client.focus.filter` rule; the compositor's new-map
    focus, left authoritative) — that is how a spawned terminal gets
    it, and why a tray-returning app's re-map takes focus as in
    awesome.
  - An activation ask (xdg-activation, the token-validated ask) NEVER
    takes the focus with `misc:focus_on_activate` off (the fork
    default, the live config): the asking window's task chip gets the
    urgent tint — a fork extension over upstream Hyprland, which
    ignores such asks silently. A chip click, `Mod+U` (`hl.dsp.focus({
    urgent_or_last = true })` — the urgent window wins over the
    last-window fallback and the view follows it to its workspace,
    awesome's `awful.client.urgent.jumpto`), or a tray/notification
    click completes the attention.
  - A focused, visible window that asks is a no-op, in both gate modes
    (awesome's `permissions.urgent` never marks the focused client;
    marking it would tint the active chip while the user watches it).
  - An ask on a not-visible window (another workspace, or minimized) is
    urgency only: no focus, no workspace switch — awesome's
    `isvisible` branch, performed in the fork's `CWindow::activate`
    (`m_workspace->visible()` is the fork's own idiom for "on a
    monitor"). An ask on a MINIMIZED window tints the chip; the
    restore is a user action (chip click, `Mod+Ctrl+N`).
  - Focus clears the urgency mark (EWMH / FS#1310; the fork's
    FocusState does it), as in awesome's client focus handler.
  - `focus_on_activate = true` switches to vanilla awesome / KWin-token
    behavior: an ask on a VISIBLE window focuses it (the not-visible
    and minimized branches above are unchanged). The user's live config
    leaves the value at the fork default (attention-only); the nested
    gate battery tests that mode.
  - X11 `_NET_ACTIVE_WINDOW` is urgency ONLY in both gate modes (the
    fork's XWM, documented there): X11 cannot authenticate a gesture,
    and Wine/Proton send it on every internal SetForegroundWindow. A
    user-initiated activation is performed by the plugin instead: tray
    and notification clicks focus the app's own window compositor-side
    (core/activate.hpp), for every backend — also the fallback when a
    Wayland sender never spends the token the plugin minted for it
    (the plugin makes awesome's "the app activates itself after the
    click" unconditional).
- **Spawn placement** is per-class: the last free geometry from the
  store, else least-overlap for a second window of the same class.
  Resizable xdg-toplevels can receive the remembered size in the
  initial configure. Client size limits, fullscreen/maximize state,
  pending native requests, rules, parent-anchored dialogs, X11
  geometry, and override-redirect surfaces stay authoritative. The close
  is remembered however the client tears down (GTK and Firefox destroy
  their toplevel before the window unmaps): the size limits are read from
  the compositor's cached copy, not the live toplevel. A
  fixed-size toplevel (min == max — a dialog or splash) keeps the
  compositor's native centered placement and never reads or writes the
  class row.
- **Snap**: during a floating move drag, nearby window and workarea
  edges pull together within `snap_distance`; reaching one output edge
  previews a half, two edges that corner's quarter. Release commits the
  preview regardless of whether the pointer button or `Super` is
  released first. Previews honor the client's size limits and keep the
  chosen edge anchored; gaps between outputs do not arm a zone.
  Disable Hyprland's native `general:snap` to avoid competing
  policies.

### Notifications (notify)

Cards render top-right on the focused monitor — the stack follows a
keyboard or workspace focus flip, but not one the pointer itself makes,
so a mouse grazing the neighboring monitor's corner leaves the cards
where they are. Newest at the top, glass·ink skin (frosted graphite,
superellipse corners).

**Spec surface.** Methods `Notify`, `CloseNotification`,
`GetCapabilities`, `GetServerInformation` (spec 1.3); signals
`NotificationClosed`, `ActionInvoked`, `ActivationToken`,
`NotificationReplied`. Capabilities: `actions`, `action-icons`,
`body`, `body-markup`, `body-hyperlinks`, `body-images`,
`icon-static`, `inline-reply`, `persistence`, `sound`.

- `replaces_id` updates a card in place, keeping its stack slot; an
  unknown id creates the card under that id. The 9990s are the private
  OSD band: a FRESH (non-replacing) id chosen inside it only sticks
  when the hints carry `x-notify-osd` (bool), so an ordinary client
  cannot pin a band id and hijack the OSD that replaces it. Fresh ids
  outside the band are minted from a low counter.
- `expire_timeout`: 0 → sticky, >0 → ms. −1 (server decides) → normal
  and critical cards are sticky until dismissed — a message waits to be
  read; self-declared ephemerals (low urgency, `transient`, `value`
  cards) run `timeout_low`. `timeout_normal` > 0 restores a clock for
  the rest.
- Hints honored: `urgency`; `value` (0–100) draws the progress bar (the
  volume/brightness OSD); `image-data`/`image_data`/`icon_data` raw
  pixmaps; `image-path`/`image_path`; `desktop-entry`; `action-icons`;
  `resident`; `transient`; `sound-file`/`sound-name`/`suppress-sound`;
  `x-notify-osd`; `x-canonical-append`; `x-notify-group-key`.
- Structured-conversation hints (neither name form sits in the
  published spec's hint table; the plain name is tried first, the
  `x-notify-*` alias second — the message-time alias alone is
  `x-notify-message-timestamp`): `conversation-id` (the merge
  contract), `conversation-title`, `conversation-kind`
  (`one-to-one`/`group`/`call`), `conversation-icon`,
  `sender-id`/`sender-name`/`sender-icon`, `message-id`,
  `message-time` (int64, ms epoch), `message-historic` (bool),
  `unread-count` (uint, capped at 999).

**Conversations.**

- A `conversation-id` in the hints is the merge contract: every
  message of that chat is ONE card however many `Notify` calls arrive
  (a replace with the same id joins; a replace carrying a DIFFERENT
  conversation-id starts the other chat's card; a replace with none
  keeps the target's conversation for the same app). Display text and
  category alone are not a contract — two chats can share a title. The
  id is opaque and rejected above 512 bytes (clipping it would merge
  two different chats).
- When a conversation-id is in force, the card's body is the
  TRANSCRIPT: the latest kept messages, CHRONOLOGICAL — the oldest line
  leads and the newest ends the body, so an arrival lands at the bottom
  and pushes the oldest out of the top of the window. The banner
  previews the five newest messages; the shade renders the full seven
  (32 retained). Group senders are prefixed by name, all under an 8 KB
  cap that evicts from the oldest end. `message-id` upserts (an edit
  replaces in place), `message-time` orders, `message-historic` marks a
  backfill (it does not grow the unread count). `unread-count` shows
  the header pill; the header also shows a
  facepile of the distinct senders of the kept messages (max three)
  for `group` chats, each avatar its `sender-icon` or a deterministic
  initials face.
- Without a conversation-id the legacy merge stands: a fresh `Notify`
  whose app + summary matches a live card is joined onto it, bodies
  joined chronologically (newest appended at the end) under the same
  cap — triggered by the fd.o
  conversation categories (`im.*`/`call.*`, where the summary is the
  sender or the room) or `x-canonical-append`. Cards that vanish on
  expiry never merge.
- `x-notify-group-key` (opaque, ≤512 bytes) sub-keys the app's bundle:
  four or more cards of one app share ONE digest per declared group,
  and the digest, the fold and the dismissal all act on that group
  alone. A replace without the hint keeps the group for the same app.

**Markup.** Body and title render the whitelisted Pango subset
`<b> <i> <u> <span> <br>`; other tags are dropped and a stray `<`/`&`
that forms no tag or entity survives as literal text, so a
markup-aware sender and a naive one both come out right. Malformed
markup falls back to plain text. `<a href>` in the body is a
hyperlink (rewritten to a styled span and hit-tested by its
stripped-text byte offset); a click opens the URL via `xdg-open` and
leaves the card up — but not the shade, since a browser is about to
cover it. The pointer shows the hand over a link. `<img src>` in the
body renders as a thumbnail row below the text; a thumbnail that fails
to load keeps its `alt` text as a one-line body entry.

**Images and icons.** Precedence: `image-data` beats `image-path`
beats `app_icon` beats `desktop-entry`. Each of `app_icon` /
`image-path` / `desktop-entry` may be a file path (`file://` too) OR a
freedesktop icon name, resolved against the GTK icon theme, then
hicolor, then `/usr/share/pixmaps`. A `desktop-entry` hint additionally
resolves the .desktop file's own `Icon=` (all `$XDG_DATA_DIRS/
applications` entries are indexed once at plugin start by a bounded
helper process, keyed by the file name and `StartupWMClass`), which
beats an icon-name collision; while the index has not reached the
entry, the icon-name stand-in draws and the index upgrades the live
card when it lands. Decoding is hyprgraphics: PNG/JPEG/WEBP/BMP/AVIF/
JXL + SVG (no GIF); big images downscale once at load, not per frame.
File icons decode off the render thread on the compositor's async
resource gatherer (a bounded share of its queue; symbolic icons and SVG
heroes stay synchronous): a card whose icon is still decoding shows
its fallback face until the decode lands and re-warms. Screenshot-sized
images (256 px in both dimensions) render card-width as a
cover-cropped hero in any capture shape. Iconless cards draw a random
face from `fallback_icon_dir`, and when that dir is empty (or a face
fails to load) a deterministic generic application mark, so no card is
faceless. The icon column is Android's conversation container: the
CONTENT image leads as the avatar (a true circle for a conversation, a
squircle otherwise) and the IDENTITY (`app_icon`/`desktop-entry`)
rides its bottom-right corner as a badge sized by AOSP's ratios off the
40 dp avatar — a 20 dp badge of which 16 dp is the app glyph and 2 dp
on each side is the rim, so only the rim protrudes. A card with no
content image leads with its identity and wears no badge. One column
says both who sent it and which app carried it.

**Actions.** Non-`default` actions render as a clickable button row; a
left click emits `ActionInvoked` and dismisses the card unless the
`resident` hint holds it. Under the `action-icons` hint each action id
is a freedesktop icon name drawn on the button. The `default` action
(and a lone action) fire on a body left click, on BOTH surfaces, and
are never drawn as a button. `ActivationToken` precedes each invoke (a
compositor-minted xdg-activation token, spec 1.3 ordering); the invoke
also focuses the sender's own window itself, for every backend — a
Wayland sender that spends the token lands on its already-focused
window (vanilla), a Wayland sender that never does, and any X11 sender
(whose own activation is unauthenticated and maps to urgency), still
get the window the click meant (the focus contract, README focus
semantics).

**Behavior.**

- Popup clicks: left invokes the action / opens the link / fires the
  default, then dismisses; right dismisses; middle parks the stack into
  the shade. The cards own the pointer over them — hover never leaks to
  the window beneath.
- Popup hover HOLDS the timeout: the card under the pointer stops
  counting down, and leaving restarts its full clock rather than
  resuming the sliver that was left. Only one card can be held, because
  only one can be hovered.
- Shade clicks: a row behaves as its banner did — left on the body
  fires the card's primary and dismisses unless `resident`, a link
  opens, a button acts. Rows open by default, so the click is spent
  acting rather than revealing; the CHEVRON is the only fold target.
  Right dismisses; middle is "Clear all". On an app bundle left
  expands and right (or the header ✕) dismisses the whole app.
- Acting CLOSES the shade: firing a card's primary, pressing one of its
  buttons or opening a body link all raise something over the panel
  the click was made in, so the panel gets out of the way. fd.o has no
  `isActivity`, and `resident` is the nearest thing it does have, so
  the shade goes exactly when the card goes. Everything that keeps you
  here keeps the shade: a dismissal, a fold, DND, "Clear all", the
  reply field, and a card with no action to fire.
- The close returns the absorbed stack: opening the shade (and a
  popup's middle-click park) stands the live banners down into parked
  shade rows. An EXPLICIT close — an outside click, Esc, the toggle —
  pops those cards back to banners, one per app under the same cap the
  DND resume applies, on fresh timeouts; a close that leaves the
  notifications invisible is a lost-notification bug. An
  action-driven close (the sender is coming up over the parked stack)
  does not re-pop.
- Inline reply (KDE's protocol, which Telegram Desktop speaks): an
  action keyed `inline-reply` is not a button — it grows a reply field
  in the open shade row, and sending emits `NotificationReplied(id,
  text)` and closes the card unless `resident`.
  `x-kde-reply-placeholder-text` and `x-kde-reply-submit-button-text`
  are honored. The field takes the whole keyboard while armed (there is
  no focus to give it); editing is append-and-backspace plus C-u / C-w.
  Banners have no field.
- Shade keys, while it is open and only then: Esc closes, ↑/↓ move a
  selection (an accent hairline; the page follows it), Space folds,
  Enter fires the primary, Tab arms the selected card's reply field,
  Delete dismisses. Modified chords pass through as user binds, and so
  does any key with nothing to act on.
- Swipe: a horizontal wheel on a row, away to dismiss. It goes through
  the click queue rather than acting in the emission (crash class 6) —
  a swipe is an alias for a click that already exists.
- Bell: a click speaks Toggle (opening absorbs the banners into parked
  rows, a later explicit close re-pops them); pointer hover is a no-op.
- Over fullscreen: banners show over a real fullscreen window too — the
  ecosystem default, no quiet-while-fullscreen policy. While a card is
  up over a solitary fullscreen window, the monitor's scanout/solitary
  latch is dropped so the card composites over it; it self-heals once
  the last card clears.
- Critical: urgent-colored frame and progress fill, never expires.
- Sound: `sound-file`/`sound-name` play through a player
  (`sound_command`, empty disables); `suppress-sound` mutes one
  arrival. The compositor has no audio backend, so this shells out,
  reaped off the event loop.
- DND (`hl.plugin.awesome.suspend()`): arrivals collect
  silently with timeouts held; resume renders the queue newest-first on
  fresh timeouts.
- Residency (`persistence`): an expired banner RETREATS into the shade
  rather than closing, and waits there until dismissed or acted on —
  the shade is the safety net. There is no history and no recall: a
  dismissed card is gone. `hyprctl awesome count` answers the live
  total (the lockscreen bell reads it).
- Session lock: cards never render above the lockscreen. Input
  listeners guard-and-reset first; whatever survives the lock repaints
  at unlock.
- Overflow: `max_notifs` caps the model — overflow evicts the oldest
  non-critical card (critical last) with `NotificationClosed`.

**Limitations.** GIF images don't decode (hyprgraphics has no GIF
codec). No animated icons (`icon-multi`). Icon-theme resolution is a
pragmatic scan (GTK theme → hicolor → pixmaps), not a full
`index.theme` inheritance engine.

### System

- **Brightness** reads `/sys/class/backlight`, applies 5% linear steps
  with a raw floor of 2, and writes through logind
  `Session.SetBrightness` on the system bus — no root, no udev rule.
- **Volume and microphone** use bounded `wpctl` set/readback chains.
  PipeWire process I/O stays off render and input callbacks (the core
  Jobs pool: pidfd-watched, capped, generation-checked); readback
  rejects malformed or non-finite output.
- **Touchpad policy**: the touchpad turns off while an external
  (USB/Bluetooth) mouse is present and back on when it's unplugged;
  `XF86TouchpadToggle` flips it by hand. Hotplug rides the
  compositor's own device signals (a settle timer coalesces the
  burst); the flip writes the compositor's per-device config store
  through `hl.device`, so nothing fights the next config re-apply, and
  a reload re-checks.
- Every action's feedback is a card posted straight into the notify
  model: fixed ids (pad 9991, volume 9992, mic 9993, brightness 9995)
  replace in place, low urgency, 1200 ms expiry, a `value` bar, and the
  matching identity icon.

## Lua API

Flat under one namespace — `hl.plugin.awesome.<fn>`:

```lua
hl.plugin.awesome.menubar()                -- the launcher
hl.plugin.awesome.maximize()               -- focused window
hl.plugin.awesome.minimize()
hl.plugin.awesome.restore()
hl.plugin.awesome.focus_next()
hl.plugin.awesome.focus_prev()
hl.plugin.awesome.focus_prev_here()
hl.plugin.awesome.center()                 -- toggles the shade
hl.plugin.awesome.suspend()                -- toggles DND
hl.plugin.awesome.clear_all()
hl.plugin.awesome.volume_up()              -- volume_down, mute, mic_mute
hl.plugin.awesome.brightness_up()          -- brightness_down
hl.plugin.awesome.touchpad_toggle()
```

## hyprctl

`hyprctl awesome <verb>` routes to the owning module: notify serves
`count`, `center`, `state`, `badge`, `topline`, `clear`; system serves
`pad`.

## Configuration

47 keys under `plugin:awesome:<module>:<key>` — shell 20, notify 24,
windows 3. The table (kinds, defaults, clamps) is
[`core/schema.cpp`](core/schema.cpp); the defaults are the glass·ink
theme tokens, so an empty config still looks right.

| module | keys |
|---|---|
| shell | `height`, `font_size`, `tray_spacing`, `font`, `terminal`, `col_bg`, `col_fg`, `col_focus`, `col_active`, `col_active_bg`, `col_on_active`, `col_empty`, `col_urgent`, `col_urgent_bg`, `col_square_sel`, `col_square_unsel`, `col_frame`, `col_charging`, `col_low`, `col_powersave` |
| notify | `font`, `font_size`, `width`, `max_height`, `max_icon`, `margin`, `offset_y`, `timeout_low`, `timeout_normal`, `coalesce_popups`, `max_notifs`, `ignore_dbusclose`, `rounding`, `rounding_power`, `sound_command`, `fallback_icon_dir`, `col_bg`, `col_fg`, `col_title`, `col_kicker`, `col_frame`, `col_urgent`, `col_highlight`, `col_link` |
| windows | `edge`, `snap_distance`, `col_frame` |

Colors and fonts arrive from the user's `theme.lua`; the C++ defaults
mirror it.

## State

One file: `$XDG_STATE_HOME/hyprland/plugin/awesome/state.tsv` — typed
tab-separated rows in fixed order (`spot`, `windowed`, `launches`,
`history`), one atomic write (temp + rename), every row admitted or
skipped (a hostile file can never take the session down). The four
module stores (spawn spots, last-windowed boxes, launcher counts,
prompt history) live in this one file; modules reach them through the
core's `StateStore` (the documented cross-module seam). First run
migrates the old layouts once — the four-file layout (both the current
and the pre-rename `awesome/` dir) and the ancient `hyprplace` /
`hyprmax` / `hyprbar` stores — merges key-by-key (newer wins), then
CONSUMES the sources: the migration is one-time, and the plugin has
exactly one state file from then on.

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
against a throwaway nested compositor; the input fixtures it drives
(vptr, vkbd, cliphold, …) are the shared `devtools/`. For one-off
diagnosis, `gate/probe.sh` runs the same preflight and then executes a
probe body script against the live nested (harness + lib already
sourced; teardown always runs).

The nested itself is a real second compositor in a window of the live
session (wayland backend), parked on an off-screen `nested-dev` output
with a private dbus session — it never touches your workspace. The
harness scripts are in `gate/` and run from a clone as-is:

```sh
bash awesome/gate/launch.sh        # start the nested (the gate uses the same)
bash awesome/gate/shot.sh out.png  # screenshot it + a 2x bar crop
bash awesome/gate/dev.sh           # stop + build + launch + probe + shot
bash awesome/gate/stop.sh          # tear it down (kill + remove the output)
```

Runtime state (signature, socket, log, gate scratch) goes to
`$HYPR_HARNESS`, default `~/.local/share/hypr-nested` — the repo stays
free of state. Never rebuild the plugin while a nested has its `.so`
mapped (`dev.sh` stops first).
