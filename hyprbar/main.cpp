// hyprbar — the AwesomeWM wibar as a native Hyprland plugin.
//
// A flat bar drawn by the compositor itself in each monitor's reserved top
// strip (the config reserves it: hl.monitor reserved = { top = <height> }).
//
//   [taglist 一..九] [tasklist of the active workspace ...] [tray] [bat] [clock] [layoutbox]
//
// - taglist: kanji buttons, awesome's exact state matrix — the viewed tag
//   gets the focus colors, urgent ones the urgent colors, everything else
//   the plain text color with occupancy shown as the little corner square
//   (filled = the tag holds the focused window, hollow = occupied).
//   Click views a tag, Mod+click sends the focused window there (silent),
//   wheel views the next/previous tag (wrapping). viewtoggle/toggle_tag
//   have no analog — a window sits on exactly one workspace.
// - tasklist: every window on THIS monitor's active workspace in arrival
//   order (stable across raises, like awesome), app icon + "⌃"/"+"/"✈"
//   state markers + title; the focused task is accent-colored text on the plain
//   bar (their tasklist_bg_focus WAS the bar bg), urgent gets the urgent
//   bg; minimized tasks are muted (awesome's fg_minimize) but keep their
//   row. Click the focused task to minimize it, click any other (minimized
//   included) to restore + focus (awesome's tasklist button, both halves);
//   right-click opens the all-clients menu (awful.menu.client_list: icons +
//   titles, click jumps to the window, its workspace included), wheel walks
//   focus through the tasks (skipping minimized). Icons resolve from the GTK
//   icon theme + hicolor + pixmaps, PNG
//   or SVG (librsvg); *-symbolic SVGs are repainted with the bar
//   foreground.
// - tray: an in-compositor StatusNotifierWatcher/Host (sdbus-c++), with a
//   native dbusmenu renderer. Left click Activates (or opens the menu for
//   menu-only items), middle SecondaryActivates, right always opens the
//   menu. SNI Status is honored: Passive hides, NeedsAttention swaps the
//   icon set. Menus behave like the GTK ones these were under X11 —
//   submenus cascade beside their parent on hover (225ms popup delay) or
//   click, over-tall panels scroll (wheel / ▴▾ strips), open levels track
//   the spec's update signals, check/radio state draws in a leading
//   column, disposition warning/alert rows take the urgent color, labels
//   honor the "__" escape.
// - battery: Android's expressive battery (the Pixel pill), transcribed
//   1:1 from SystemUI's Compose implementation and drawn natively in the
//   warm pass (cairo; assets embedded verbatim, see battery.cpp) — digits
//   inside, Android's attribution ladder to the right (power-save plus >
//   charge-limit shield > charging bolt > the D cap) and its fill colors:
//   yellow in power save, green charging OR held at the charge limit,
//   error red at 20% discharging, white otherwise. State from
//   /sys/class/power_supply + /sys/firmware/acpi/platform_profile
//   (hidden on desktops). The old battery-watch.sh alerts live here too: AC
//   plug/unplug, low (20%) and critical (5%, Android's lines) —
//   edge-triggered, riding the same udev uevents as the gauge, sent as
//   direct Notify calls over the tray's bus connection (no fork;
//   hyprnotify answers from the same process).
// - clock: "%a %b %d, %H:%M" (the awesome textclock default; the bar pads
//   it with a real margin, not the format's literal spaces).
// - layoutbox: rightmost like awesome — the active workspace's layout
//   icon (~/.config/hypr/icons/<name>.png), per-tag state like awesome's.
//   awesome's buttons: click next, right-click previous, wheel both ways;
//   Super+Space / Super+Shift+Space call layout_next()/layout_prev().
//   The registry holds one layout (floating) until more are implemented.
// - menubar: awesome's Mod+P launcher in its OWN strip right below the
//   bar — the bar stays visible, exactly like awesome's menubar wibox at
//   the workarea top (hl.plugin.hyprbar.menubar()): "Run: " prompt, the
//   11 awesome categories (Enter drills in, BackSpace/Escape on empty
//   backs out) and the .desktop apps, filtered as you type — name or
//   command line, substring, prefix matches and most-launched entries
//   first — plus a trailing "Exec: <query>" entry that runs whatever was
//   typed. Left/Right or C-j/C-k select, Home/End jump, Enter runs
//   (Terminal=true entries in plugin:hyprbar:terminal), C-Return runs the
//   raw query, C-M-Return runs it in the terminal, Tab/Shift-Tab cycle
//   shell completion ($PATH for the command word, filenames after),
//   Up/Down or C-p/C-n walk the prompt history, readline editing
//   (C-a/e/b/f/d/h/u/w, M-b/f/d, C-BackSpace), Escape or any click
//   closes. Entries show a theme icon when one resolves, else the
//   tasklist's letter fallback — the icon cell always reserved so rows
//   keep their rhythm (a deliberate step past awesome's collapsing
//   imagebox). Launch counts and history persist in ~/.cache/hyprbar/
//   (menu_count_file, history_menu), like awesome's.
// - The strip owns the pointer: hovering the bar never leaks the cursor
//   shape or hover focus to a window poking underneath it.
// - The bar hides while the workspace has a real fullscreen window
//   (maximized windows respect the reserved strip and keep it visible).
//
// Colors/fonts arrive from theme.lua via hl.config plugin values — the C++
// defaults just mirror the theme.
//
// THE TEXTURE RULE, the one thing to know before touching the bar or a widget: a
// texture cannot be painted by the frame that created it, and creating one
// mid-draw silently swallows every later draw in the same element. So
// renderBar is one layout with two modes — warm builds every texture and
// paints nothing, draw paints and never builds — and warmBars() runs the warm
// from the EVENT LOOP, a frame ahead of the paint that needs it. Everything
// that changes the bar goes through barChanged()/damageAndWarm() rather than
// damageBars() alone. Ignoring this is what made the tasklist vanish for one
// frame on every window open/close.
//
// Perf notes: the bar renders only when its monitor renders (damage-driven,
// zero cost idle); per-frame hit rebuilding is allocation-free (POD hits in
// a reused vector); one walk of the window list feeds both the taglist's
// urgency and the tasklist; text/icons are cached as GPU textures, kept by a
// warm-generation grace rather than a size cap — evicting to just the current
// layout would thrash, since sloppy focus re-keys two task labels for every
// window the cursor crosses. Tray property replies are
// change-detected — fcitx fires NewIcon per input-context change (= every
// window focus), and unconditional rebuilds made the bar visibly reload on
// every window action. Icon name+pixmap are fetched serially and committed
// as one change (separate replies = a mismatched intermediate frame on every
// REAL fcitx idle<->unikey flip), and name-resolved tray textures are cached
// so a flip never touches the disk. DBus is event-driven: sd-bus's fds live
// in the wayland event loop as removable sources (torn out before the
// connection dies), so idle costs zero wakeups and a tray signal lands the
// same loop iteration; a normally-disarmed timer carries only sd-bus's own
// rare timeouts and the deferred post-send drain (dispatch is not
// re-entrant). The clock re-arms to the minute it actually changes on, and
// the battery gauge refreshes from power_supply udev uevents (plug/unplug is
// instant) with the minute tick as a failsafe; the plug/low/critical alerts
// ride the same two paths.
//
// The code is split by concern — the skeleton hosts widgets, each in its own
// unit next to the state it paints from; see hyprbar.hpp for the module map.

#include "common/lifecycle.hpp"
#include "common/order.hpp"

#include "hyprbar.hpp"

using namespace NHyprbar;

HANDLE PHANDLE = nullptr;

namespace NHyprbar {
    SBarConfig cfg;
}

// the minute tick: clock text + battery failsafe read
static SP<CEventLoopTimer>                                 timer;

static NHyprCommon::CLifecycle g_lifecycle;

static NHyprCommon::CHop       pendingWarm;

// Anything that changes what the bar shows lands here. The textures are built
// NOW, in the event loop, because a texture built during a frame cannot be
// painted by that frame — lazily building them inside the render swallowed the
// whole tasklist for one frame on every reflow. Deferred so the window/
// workspace state the layout reads is already settled.
static void damageAndWarm() {
    if (!g_pEventLoopManager) {
        damageBars();
        return;
    }
    pendingWarm.arm([]() {
        warmBars();
        damageBars();
    });
}

// The clock shows minutes, so it only needs to tick on the minute — arm to
// the next boundary (+ a margin to land just past it) instead of every 10s.
static std::chrono::milliseconds toNextMinute() {
    const auto NOW = std::time(nullptr);
    return std::chrono::milliseconds((60 - NOW % 60) * 1000 + 200);
}

// hl.plugin.hyprbar.menubar() — awesome's Mod+P, the strip below the bar.
static int luaMenubar(lua_State*) {
    Menubar::toggleDeferred();
    return 0;
}

// hl.plugin.hyprbar.layout_next/layout_prev() — awesome's awful.layout.inc(±1).
static int luaLayoutNext(lua_State*) {
    layoutInc(1);
    return 0;
}
static int luaLayoutPrev(lua_State*) {
    layoutInc(-1);
    return 0;
}

// hl.plugin.hyprbar.minimize()/restore() — awesome's client.minimized (Mod+N)
// and awful.client.restore (Mod+Ctrl+N). Deferred out of the keybind emission:
// both change focus + layout, which must never run synchronously inside an
// input emission; the pending hops are reset in PLUGIN_EXIT.
static NHyprCommon::CHop pendingMinimize, pendingRestore, pendingUnfocusHidden;
static int               luaMinimize(lua_State*) {
    pendingMinimize.arm([]() { Tasklist::minimizeFocused(); });
    return 0;
}
static int luaRestore(lua_State*) {
    pendingRestore.arm([]() { Tasklist::restoreLast(); });
    return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprbar] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprbar] version mismatch");
    }

    // the bar swallows its strip clicks and opens tray menus before they
    // count as window clicks anywhere else
    NHyprCommon::mustLoadBefore(PHANDLE, "hyprbar", {"hyprnotify", "hyprmax", "hyprclick"});

    // Defaults mirror theme.lua; the config overwrites them from the theme.
    cfg.height         = makeShared<Config::Values::CIntValue>("plugin:hyprbar:height", "bar height in logical px (reserve it: monitor reserved top)", 26);
    cfg.fontSize       = makeShared<Config::Values::CIntValue>("plugin:hyprbar:font_size", "text size in logical px (monitor scale applies at raster time)", 12);
    cfg.traySpacing    = makeShared<Config::Values::CIntValue>("plugin:hyprbar:tray_spacing", "px between tray icons (awesome systray_icon_spacing)", 10);
    cfg.font           = makeShared<Config::Values::CStringValue>("plugin:hyprbar:font", "font family", "Fira Code");
    cfg.terminal       = makeShared<Config::Values::CStringValue>("plugin:hyprbar:terminal", "terminal that runs Terminal=true menubar entries", "alacritty");
    cfg.colBg          = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_bg", "bar background", 0xff131313);
    cfg.colFg          = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_fg", "normal text", 0xffaaaaaa);
    cfg.colMuted       = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_muted", "tray letter fallback", 0xff8a97a8);
    cfg.colFocus       = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_focus", "selected menubar entry text (awesome fg_focus)", 0xff32d6ff);
    cfg.colActive      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_active", "active tag / focused task text", 0xff00ccff);
    cfg.colActiveBg    = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_active_bg", "active tag background", 0xff1e2320);
    cfg.colEmpty       = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_empty", "disabled/placeholder text", 0xff565e6b);
    cfg.colUrgent      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_urgent", "urgent text", 0xffc83f11);
    cfg.colUrgentBg    = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_urgent_bg", "urgent background (awesome bg_urgent)", 0xff3f3f3f);
    cfg.colSquareSel   = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_square_sel", "taglist square, tag holds the focused window", 0xfff0dfaf);
    cfg.colSquareUnsel = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_square_unsel", "taglist square, occupied tag", 0xffdcdccc);
    cfg.colFrame       = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_frame", "menu panel frame", 0xff3f3f3f);
    cfg.colCharging    = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_charging", "battery fill charging/defending (Android's charging green)", 0xff18cc47);
    cfg.colLow         = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_low", "battery fill at 20% and under (Android's error red)", 0xffff0e01);
    cfg.colSave        = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_powersave", "battery fill in power save (Android's warning yellow)", 0xffffc917);

    for (const auto& V : {cfg.height, cfg.fontSize, cfg.traySpacing})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.font, cfg.terminal})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colMuted, cfg.colFocus, cfg.colActive, cfg.colActiveBg, cfg.colEmpty, cfg.colUrgent, cfg.colUrgentBg, cfg.colSquareSel,
                          cfg.colSquareUnsel, cfg.colFrame, cfg.colCharging, cfg.colLow, cfg.colSave})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    buildIconDirs();
    Clock::refresh();
    Battery::init();
    Tray::init();

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.render.stage, [](eRenderStage stage) { onRenderStage(stage); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.move, [](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.axis, [](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.keyboard.key, [](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { Menubar::onKey(e, info); });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "menubar", luaMenubar);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "layout_next", luaLayoutNext);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "layout_prev", luaLayoutPrev);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "minimize", luaMinimize);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "restore", luaRestore);

    // Anything that changes what the bar shows -> damage the strip.
    auto& EV = Event::bus()->m_events;
    g_lifecycle.listen(EV.window.open, [](PHLWINDOW w) {
        Tasklist::watchMinimize(w); // honor the client's own set_minimized
        damageAndWarm();
    });
    g_lifecycle.listen(EV.window.close, [](PHLWINDOW) { damageAndWarm(); });
    g_lifecycle.listen(EV.window.destroy, [](PHLWINDOWREF w) {
        Tasklist::forget(w.get());
        damageAndWarm();
    });
    g_lifecycle.listen(EV.window.active, [](PHLWINDOW w, Desktop::eFocusReason) {
        damageAndWarm();
        // awesome's check_focus: focus must never rest on a minimized window.
        // The compositor's focus fallback (closing the last visible window)
        // lands focus on a hidden one; bounce it off, deferred out of the focus
        // emission (refocusing inside it reenters the focus machinery).
        if (w && w->isHidden() && Tasklist::isMinimized(w)) {
            PHLWINDOWREF WR{w};
            pendingUnfocusHidden.arm([WR]() {
                if (const auto W = WR.lock())
                    Tasklist::focusAwayFromHidden(W);
            });
        }
    });
    g_lifecycle.listen(EV.window.title, [](PHLWINDOW w) {
        // a hidden workspace's titles render nowhere; workspace.active re-warms the switch
        if (w && w->m_workspace && !w->m_workspace->isVisible())
            return;
        damageAndWarm();
    });
    g_lifecycle.listen(EV.window.urgent, [](PHLWINDOW) { damageAndWarm(); });
    g_lifecycle.listen(EV.window.pin, [](PHLWINDOW) { damageAndWarm(); });      // the tasklist's ⌃ marker
    g_lifecycle.listen(EV.window.floating, [](PHLWINDOW) { damageAndWarm(); }); // the ✈ marker
    g_lifecycle.listen(EV.window.class_, [](PHLWINDOW) { damageAndWarm(); });   // the task icon re-resolves
    g_lifecycle.listen(EV.window.fullscreen, [](PHLWINDOW) { damageAndWarm(); });
    g_lifecycle.listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.active, [](PHLWORKSPACE) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.created, [](PHLWORKSPACEREF) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.removed, [](PHLWORKSPACEREF) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { damageAndWarm(); });
    g_lifecycle.listen(EV.monitor.layoutChanged, []() { damageAndWarm(); });

    timer = makeShared<CEventLoopTimer>(
        toNextMinute(),
        [](SP<CEventLoopTimer> self, void*) {
            const bool CLK = Clock::refresh(), BAT = Battery::refresh();
            if (CLK || BAT)
                damageAndWarm();
            Battery::alerts();
            self->updateTimeout(toNextMinute());
        },
        nullptr);
    g_pEventLoopManager->addTimer(timer);

    damageBars();

    return {"hyprbar", "the awesome wibar, drawn by the compositor", "hitori", "2.2.5"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop, .so-wide
    Menubar::exit();
    Menu::exit();
    Tray::exit();
    inputExit();
    Battery::exit();
    if (timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(timer);
    timer.reset();
    renderExit();
    layoutboxExit();
    Tasklist::exit();
    Clock::exit();
    iconsExit();
    damageBars();
}
