// awesome/shell.cpp — the shell module: the bar glue.
//
// init owns the surface units (strip, icons, clock, battery, tray, bell,
// menubar), the window/view listeners, the minute timer, and the Lua face.
// The input pipeline face forwards to shell/input.cpp; the windows module
// owns the window-state listeners (minimize, arrival, focus guards) and the
// shell only watches what it paints.
#include "shell.hpp"

#include "shell/shell.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace NAwesome::Shell {

    CModule& module() {
        static CModule M;
        return M;
    }

    namespace {
        SP<CEventLoopTimer> timer;

        // the clock/battery minute tick: the next wall-clock minute, +200ms so
        // the refresh lands after the clock has actually rolled over
        std::chrono::milliseconds toNextMinute() {
            const auto NOW = std::time(nullptr);
            return std::chrono::milliseconds((60 - (int)(NOW % 60)) * 1000 + 200);
        }
    } // namespace

    int luaMenubar(lua_State*) {
        Menubar::toggleDeferred();
        return 0;
    }

    void CModule::init() {
        buildIconDirs();
        stripInit(); // the strip's canvas layer before anything warms
        iconsInit();
        Clock::refresh();
        Battery::init();
        Tray::init();
        Bell::init(); // the badge hook: the model's funnel repaints the strip
        Menubar::init();

        auto& EV = Event::bus()->m_events;
        // Anything that changes what the bar shows -> warm + damage the
        // strip. The STATE bookkeeping (arrival stamping, the minimize
        // listeners, the focus guards, the forgets) is the windows
        // module's — it registers before this module's listeners see the
        // same events.
        supervisor().listen(EV.window.open, [](PHLWINDOW) { barChanged(); });
        supervisor().listen(EV.window.close, [](PHLWINDOW) { barChanged(); });
        supervisor().listen(EV.window.destroy, [](PHLWINDOWREF) { barChanged(); });
        supervisor().listen(EV.window.active, [](PHLWINDOW, Desktop::eFocusReason) { barChanged(); });
        supervisor().listen(EV.window.title, [](PHLWINDOW w) {
            // a hidden workspace's titles render nowhere; workspace.active re-warms the switch
            if (w && w->m_workspace && !w->m_workspace->visible())
                return;
            barChanged();
        });
        supervisor().listen(EV.window.urgent, [](PHLWINDOW) { barChanged(); });
        supervisor().listen(EV.window.pin, [](PHLWINDOW) { barChanged(); });         // the tasklist's ⌃ marker
        supervisor().listen(EV.window.floating, [](PHLWINDOW) { barChanged(); });   // the ✈ marker
        supervisor().listen(EV.window.class_, [](PHLWINDOW) { barChanged(); });     // the task icon re-resolves
        supervisor().listen(EV.window.fullscreen, [](PHLWINDOW) { barChanged(); });
        supervisor().listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { barChanged(); });
        supervisor().listen(EV.workspace.active, [](PHLWORKSPACE) { barChanged(); });
        supervisor().listen(EV.workspace.created, [](PHLWORKSPACEREF) { barChanged(); });
        supervisor().listen(EV.workspace.removed, [](PHLWORKSPACEREF) { barChanged(); });
        supervisor().listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { barChanged(); });
        supervisor().listen(EV.monitor.layoutChanged, []() { barChanged(); });

        // Colors and fonts heal themselves — they are part of every texture's
        // cache key, so a changed value simply misses and rebuilds. What
        // doesn't is anything resolved from DISK at init: the icon dirs
        // (probed once from the GTK theme name) and the files found in them.
        // Desktop discovery is worker-owned and independent of the theme, so
        // a config reload does not restart it.
        supervisor().listen(EV.config.reloaded, []() {
            iconsReload();
            for (const auto& I : Tray::items)
                I->dirty = true; // their textures came out of the theme we just dropped
            barChanged();
        });

        timer = makeShared<CEventLoopTimer>(
            toNextMinute(),
            [](SP<CEventLoopTimer> self, void*) {
                const bool CLK = Clock::refresh(), BAT = Battery::refresh();
                if (CLK || BAT)
                    barChanged();
                Battery::alerts();
                self->updateTimeout(toNextMinute());
            },
            nullptr);
        g_pEventLoopManager->addTimer(timer);

        HyprlandAPI::addLuaFunction(supervisor().handle(), "awesome", "menubar", luaMenubar);

        damageBars();
    }

    void CModule::teardown() {
        // reverse of init; the supervisor has already dropped the listeners
        // and every hop, so the units below only unwind their own state
        if (timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(timer);
        timer.reset();
        Menubar::exit();
        Menu::exit();
        Tray::exit();
        Bell::exit();
        Battery::exit();
        inputExit();
        stripExit();
        iconsExit();
        Clock::exit();
        damageBars();
    }

    // ---- the pipeline (the bar claims the strip; notify and windows run
    // after it, system last) ----

    void CModule::onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        onMouseButton(e, info);
    }
    void CModule::onPointerMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        onMouseMove(pos, info);
    }
    void CModule::onPointerAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        onMouseAxis(e, info);
    }
    void CModule::onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        Menubar::onKey(e, info);
    }
    void CModule::onInputBlocked() {
        Shell::onInputBlocked();
    }

} // namespace NAwesome::Shell
