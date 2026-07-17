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
//   bg. Click focuses + raises (no minimize here, so the click-focused-
//   to-minimize half of awesome's button is off), right-click opens the
//   all-clients menu (awful.menu.client_list: icons + titles, click jumps
//   to the window, its workspace included), wheel walks focus through the
//   tasks. Icons resolve from the GTK icon theme + hicolor + pixmaps, PNG
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
// - battery: the old awesome widget's face — a Material Icons Round glyph
//   (plugin:hyprbar:font_icon, charging bolt on AC else a 12.5%-step
//   gauge) + percent, from /sys/class/power_supply (hidden on desktops).
//   Alerts stay in scripts/battery-watch.sh.
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
//   closes. Entries show a theme icon when one resolves and plain text
//   otherwise, like awesome (no letter fallback). Launch counts and
//   history persist in ~/.cache/hyprbar/ (menu_count_file, history_menu),
//   like awesome's.
// - The strip owns the pointer: hovering the bar never leaks the cursor
//   shape or hover focus to a window poking underneath it.
// - The bar hides while the workspace has a real fullscreen window
//   (maximized windows respect the reserved strip and keep it visible).
//
// Colors/fonts arrive from theme.lua via hl.config plugin values — the C++
// defaults just mirror the theme.
//
// THE TEXTURE RULE, the one thing to know before touching render.cpp: a
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
// so a flip never touches the disk. DBus is polled from a 50 ms main-thread
// timer (fd integration would be nicer, but a readable-waiter can't be
// unregistered and would outlive plugin unload = crash); every async send
// pulls the next tick to 2 ms, so signal->fetch->commit chains settle in a
// few ms instead of riding whole poll periods — a tray flip lands in the
// same breath as the window event that caused it.
//
// The code is split by concern — see hyprbar.hpp for the module map.

#include "hyprbar.hpp"

using namespace NHyprbar;

HANDLE PHANDLE = nullptr;

namespace NHyprbar {
    SBarConfig cfg;
}

// the 10s clock/battery refresh
static SP<CEventLoopTimer>                                 timer;

static Hyprutils::Signal::CHyprSignalListener              lRender, lButton, lMove, lAxis, lKey;
static std::vector<Hyprutils::Signal::CHyprSignalListener> lDamage;

static UP<SEventLoopDoLaterLock> pendingWarm;

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
    pendingWarm = g_pEventLoopManager->doLaterLock([]() {
        warmBars();
        damageBars();
    });
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

    // Defaults mirror theme.lua; the config overwrites them from the theme.
    cfg.height         = makeShared<Config::Values::CIntValue>("plugin:hyprbar:height", "bar height in logical px (reserve it: monitor reserved top)", 26);
    cfg.fontSize       = makeShared<Config::Values::CIntValue>("plugin:hyprbar:font_size", "text size in logical px (monitor scale applies at raster time)", 12);
    cfg.traySpacing    = makeShared<Config::Values::CIntValue>("plugin:hyprbar:tray_spacing", "px between tray icons (awesome systray_icon_spacing)", 10);
    cfg.font           = makeShared<Config::Values::CStringValue>("plugin:hyprbar:font", "font family", "Fira Code");
    cfg.fontIcon       = makeShared<Config::Values::CStringValue>("plugin:hyprbar:font_icon", "battery glyph font (awesome font_icon)", "Material Icons Round");
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

    for (const auto& V : {cfg.height, cfg.fontSize, cfg.traySpacing})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.font, cfg.fontIcon, cfg.terminal})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V :
         {cfg.colBg, cfg.colFg, cfg.colMuted, cfg.colFocus, cfg.colActive, cfg.colActiveBg, cfg.colEmpty, cfg.colUrgent, cfg.colUrgentBg, cfg.colSquareSel, cfg.colSquareUnsel})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    buildIconDirs();
    findBattery();
    refreshTexts();
    Tray::init();

    lRender = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    lButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    lMove   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });
    lAxis   = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });
    lKey    = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { Menubar::onKey(e, info); });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "menubar", luaMenubar);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "layout_next", luaLayoutNext);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbar", "layout_prev", luaLayoutPrev);

    // Anything that changes what the bar shows -> damage the strip.
    auto& EV = Event::bus()->m_events;
    lDamage.push_back(EV.window.open.listen([](PHLWINDOW) { damageAndWarm(); }));
    lDamage.push_back(EV.window.close.listen([](PHLWINDOW) { damageAndWarm(); }));
    lDamage.push_back(EV.window.destroy.listen([](PHLWINDOWREF w) {
        winSeq.erase(w.get());
        damageAndWarm();
    }));
    lDamage.push_back(EV.window.active.listen([](PHLWINDOW, Desktop::eFocusReason) { damageAndWarm(); }));
    lDamage.push_back(EV.window.title.listen([](PHLWINDOW w) {
        // a hidden workspace's titles render nowhere; workspace.active re-warms the switch
        if (w && w->m_workspace && !w->m_workspace->isVisible())
            return;
        damageAndWarm();
    }));
    lDamage.push_back(EV.window.urgent.listen([](PHLWINDOW) { damageAndWarm(); }));
    lDamage.push_back(EV.window.pin.listen([](PHLWINDOW) { damageAndWarm(); }));      // the tasklist's ⌃ marker
    lDamage.push_back(EV.window.floating.listen([](PHLWINDOW) { damageAndWarm(); })); // the ✈ marker
    lDamage.push_back(EV.window.class_.listen([](PHLWINDOW) { damageAndWarm(); }));   // the task icon re-resolves
    lDamage.push_back(EV.window.fullscreen.listen([](PHLWINDOW) { damageAndWarm(); }));
    lDamage.push_back(EV.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { damageAndWarm(); }));
    lDamage.push_back(EV.workspace.active.listen([](PHLWORKSPACE) { damageAndWarm(); }));
    lDamage.push_back(EV.workspace.created.listen([](PHLWORKSPACEREF) { damageAndWarm(); }));
    lDamage.push_back(EV.workspace.removed.listen([](PHLWORKSPACEREF) { damageAndWarm(); }));
    lDamage.push_back(EV.workspace.moveToMonitor.listen([](PHLWORKSPACE, PHLMONITOR) { damageAndWarm(); }));
    lDamage.push_back(EV.monitor.layoutChanged.listen([]() { damageAndWarm(); }));

    timer = makeShared<CEventLoopTimer>(
        std::chrono::seconds(10),
        [](SP<CEventLoopTimer> self, void*) {
            if (refreshTexts())
                damageAndWarm();
            self->updateTimeout(std::chrono::seconds(10));
        },
        nullptr);
    g_pEventLoopManager->addTimer(timer);

    damageBars();

    return {"hyprbar", "the awesome wibar, drawn by the compositor: kanji taglist, tasklist with icons, tray with menus, menubar launcher, battery, clock", "hitori", "1.2.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    Menubar::exit();
    Menu::exit();
    Tray::exit();
    inputExit();
    if (timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(timer);
    timer.reset();
    lRender.reset();
    lButton.reset();
    lMove.reset();
    lAxis.reset();
    lKey.reset();
    lDamage.clear();
    renderExit();
    iconsExit();
    winSeq.clear();
    damageBars();
}
