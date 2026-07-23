// hyprbar — the shell bar, drawn by the compositor.
//
// TWO modes, one machinery (plugin:hyprbar:mode): the compact ISLANDS — a
// 30px transparent band in each monitor's reserved top strip (hl.monitor
// reserved = { top = <height> }) holding 26px frosted-glass pills — and the
// STRIP: one full-bleed frosted band (col_bg's RGB at bar_alpha, flat, no
// hairlines, grain + under-shadow) whose cells run the full height, so y=0
// and both corners are live click targets and the menubar docks as a second
// band row. Identical maximized or not, nothing ever relayouts. Real
// fullscreen hides the band; the open menubar floats above even that. The
// skin is glass·ink (common/theme.hpp): live blur, superellipse corners
// (islands — the strip is square).
//
//   ( 一..九 )  (chip) (chip)…      ( kbd  tray  bell  wifi  battery  HH:MM )
//
// - taglist (left island): the nine kanji. Viewed = accent on an
//   accent-dim fill; urgent = the kanji in the urgent color and nothing
//   else (viewing the tag clears it, tracked bar-side); occupied = ink;
//   empty = muted. Click views, Mod+click sends the focused window
//   (silent), wheel cycles wrapping.
// - task chips (the middle): one pill per window of the active workspace,
//   arrival order (stable across raises) — 15px themed app icon (75% at
//   rest, full ink on focus; resolver in common/icons.hpp) + "⌃"/"+"/"✈"
//   markers + title, max-w 220, chips shrink together when the strip runs
//   out. Focused chip fills accent-dim; urgent tints; minimized chips
//   stay, muted. Left = focus (focused again = minimize), middle = close,
//   wheel walks focus (skipping minimized). Minimize is the in-place
//   awesome model (hidden window, layout slot freed, FS mode held);
//   a client's own minimize request is honored.
// - the status island (right) — keyboard-layout chip · tray · bell ·
//   battery · time; gap 7, no separators, glyphs full ink with state alone
//   recoloring. (No wifi wedge: nm-applet's SNI icon in the tray already
//   carries the strength — user call 2026-07-23.)
//   - kbd chip: the active layout's two letters, off the keyboard.layout
//     event. An indicator.
//   - tray: StatusNotifierWatcher/Host (sdbus-c++), 24px cells with 15px
//     icons. Left Activates (menu-only items open their menu), middle
//     SecondaryActivates, right opens the dbusmenu — a glass panel now
//     (radius 12, h26 rows, accent-dim hover), closing on pick, outside
//     click, or esc; submenus cascade on GTK's 225ms delay, over-tall
//     panels scroll, live updates tracked, "__" labels honored.
//   - bell: Material's filled shape + the live+kept badge (hides at 0).
//     A click calls Toggle on hyprnotify's org.hitori.hyprnotify bus face;
//     the badge rides its State signal. DND has NO bar presence.
//   - battery: Android's expressive pill, transcribed 1:1 from SystemUI
//     (battery.cpp) — digits inside, the attribution ladder (defend >
//     charging > save-on-the-cell; Android disables saver on AC), fill
//     ink · accent charging/defending · urgent ≤20% · gold in power save.
//     The plug/low/critical alerts ride the same udev uevents, sent as
//     Notify calls over the tray's bus connection.
//   - time: the bold clock — plugin:hyprbar:clock_format (strftime, ticks
//     per minute; the user runs awesome's stock "%a %b %d, %H:%M").
// - menubar (hl.plugin.hyprbar.menubar()): the launcher below the band — a
//   floating glass pill in islands ("run:" prompt over a right hairline,
//   pill chips, selected = accent-dim), DOCKED in strip: a second full-width
//   band row, one tone up (col_bar_menubar), square full-height chips,
//   selected = SOLID accent. Filtering, completion, history and readline
//   editing are identical in both; counts/history persist in ~/.cache/hyprbar/.
// - The band owns the pointer: hovering it never leaks the cursor shape or
//   hover focus to a window poking underneath. A hover cell is tracked so
//   tags/chips/tray light their fills.
//
// Colors/fonts arrive from theme.lua via hl.config plugin values — the C++
// defaults ARE the glass·ink tokens.
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

    // Defaults are the glass·ink tokens (common/theme.hpp); theme.lua
    // overrides them through the same values as always.
    namespace Th = NHyprCommon::Theme;
    cfg.height        = makeShared<Config::Values::CIntValue>("plugin:hyprbar:height", "band height in logical px (islands are height-4; reserve it: monitor reserved top)", 30);
    cfg.fontSize      = makeShared<Config::Values::CIntValue>("plugin:hyprbar:font_size", "text size in logical px (monitor scale applies at raster time)", 12);
    cfg.traySpacing   = makeShared<Config::Values::CIntValue>("plugin:hyprbar:tray_spacing", "px between tray icons", 3);
    cfg.roundingPower = makeShared<Config::Values::CFloatValue>("plugin:hyprbar:rounding_power", "corner superellipse exponent", (float)Th::ROUNDING_POWER);
    cfg.barAlpha      = makeShared<Config::Values::CFloatValue>("plugin:hyprbar:bar_alpha", "strip mode: the band's glass alpha over col_bg's RGB", 0.62f);
    cfg.mode          = makeShared<Config::Values::CStringValue>("plugin:hyprbar:mode", "islands | strip (strip: one full-bleed frosted band, full-height hitboxes, docked menubar)", "islands");
    cfg.font          = makeShared<Config::Values::CStringValue>("plugin:hyprbar:font", "font family", Th::FONT);
    cfg.clockFormat   = makeShared<Config::Values::CStringValue>("plugin:hyprbar:clock_format", "strftime clock text (the clock ticks per minute)", "%H:%M");
    cfg.terminal      = makeShared<Config::Values::CStringValue>("plugin:hyprbar:terminal", "terminal that runs Terminal=true menubar entries", "foot");
    cfg.colBg         = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_bg", "island glass (alpha is the glass)", Th::GLASS);
    cfg.colFg         = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_fg", "full-ink text and status glyphs", Th::INK);
    cfg.colMuted      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_muted", "secondary text, letter fallbacks", Th::SUB);
    cfg.colFocus      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_focus", "selected menubar entry text", Th::ACCENT);
    cfg.colActive     = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_active", "active tag / focused task text (the accent)", Th::ACCENT);
    cfg.colActiveBg   = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_active_bg", "active/selected fills (accent-dim)", Th::ACCENT_DIM);
    cfg.colEmpty      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_empty", "empty tags, disabled text", 0x8098a2ac);
    cfg.colUrgent     = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_urgent", "urgent text (the urgent kanji)", Th::URGENT);
    cfg.colUrgentBg   = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_urgent_bg", "urgent chip fill", 0x29ff8a5c);
    cfg.colFrame      = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_frame", "hairlines", Th::LINE);
    cfg.colBarMenubar = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_bar_menubar", "strip mode: the docked menubar row (one tone up)", 0xa8181d26);
    cfg.colCharging   = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_charging", "battery fill charging/defending (the accent)", Th::ACCENT);
    cfg.colLow        = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_low", "battery fill at 20% and under (urgent)", Th::URGENT);
    cfg.colSave       = makeShared<Config::Values::CColorValue>("plugin:hyprbar:col_powersave", "battery fill in power save (gold)", 0xffffc917);

    for (const auto& V : {cfg.height, cfg.fontSize, cfg.traySpacing})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.roundingPower, cfg.barAlpha})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.mode, cfg.font, cfg.clockFormat, cfg.terminal})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colMuted, cfg.colFocus, cfg.colActive, cfg.colActiveBg, cfg.colEmpty, cfg.colUrgent, cfg.colUrgentBg, cfg.colFrame,
                          cfg.colBarMenubar, cfg.colCharging, cfg.colLow, cfg.colSave})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Clock::refresh();
    Battery::init();
    Tray::init();
    Bell::init(); // rides the tray's bus link
    Kbd::init();

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.render.stage, [](eRenderStage stage) { onRenderStage(stage); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.move, [](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.axis, [](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.keyboard.key, [](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
        if (onBarKey(e, info)) // esc peels the tray menu first
            return;
        Menubar::onKey(e, info);
    });
    g_lifecycle.listen(Event::bus()->m_events.input.keyboard.layout, [](SP<IKeyboard>, const std::string& name) { Kbd::onLayout(name); });

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
        Taglist::forget(w.get());
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
    g_lifecycle.listen(EV.workspace.active, [](PHLWORKSPACE ws) {
        Taglist::noteViewed(ws); // Android's "viewing clears the urgent tag"
        damageAndWarm();
    });
    g_lifecycle.listen(EV.workspace.created, [](PHLWORKSPACEREF) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.removed, [](PHLWORKSPACEREF) { damageAndWarm(); });
    g_lifecycle.listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { damageAndWarm(); });
    g_lifecycle.listen(EV.monitor.layoutChanged, []() { damageAndWarm(); });
    // plugin values land on the reparse AFTER load (and on every manual
    // reload): re-derive the clock text — its format is config — and repaint
    // with the fresh palette/mode rather than waiting out the minute tick
    g_lifecycle.listen(EV.config.reloaded, []() {
        Clock::refresh();
        damageAndWarm();
    });

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

    return {"hyprbar", "the shell bar: compact islands or the full-bleed strip", "hitori", "3.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop, .so-wide
    Menubar::exit();
    Menu::exit();
    Bell::exit(); // its proxy borrows the tray's connection — before Tray::exit
    Tray::exit();
    inputExit();
    Battery::exit();
    Kbd::exit();
    if (timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(timer);
    timer.reset();
    renderExit();
    layoutboxExit();
    Tasklist::exit();
    Taglist::exit();
    Clock::exit();
    iconsExit();
    damageBars();
}
