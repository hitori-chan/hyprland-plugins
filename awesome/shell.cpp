// awesome/shell.cpp — the shell module: the bar glue.
//
// init owns the surface units (strip, icons, clock, battery, tray, bell,
// menubar), the window/view listeners, the minute timer, and the Lua face.
// The input pipeline face forwards to shell/input.cpp; the windows module
// owns the window-state listeners (minimize, arrival, focus guards) and the
// shell only watches what it paints.
#include "shell.hpp"

#include "core/walltimer.hpp"
#include "shell/shell.hpp"
#include "windows/state.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <ctime>

namespace NAwesome::Shell {

    CModule& module() {
        static CModule M;
        return M;
    }

    namespace {
        // The clock/battery minute tick rides the wall clock (a resume
        // showed the time the laptop went to sleep for up to a minute):
        // the next minute, re-armed on every wakeup — a clock step's too.
        CWallTimer minute;

        void armMinute() {
            minute.arm(((int64_t)std::time(nullptr) / 60 + 1) * 60);
        }

        void onMinute() {
            const bool CLK = Clock::refresh(), BAT = Battery::refresh();
            if (CLK || BAT)
                barChanged();
            Battery::alerts();
            armMinute();
        }
    } // namespace

    int luaMenubar(lua_State*) {
        Menubar::toggleDeferred();
        return 0;
    }

    void CModule::init() {
        stripInit(); // the strip's canvas layer before anything warms
        iconsInit();
        Clock::refresh();
        Battery::init();
        Tray::init();
        Bell::init(); // the badge hook: the model's funnel repaints the strip
        Menubar::init();
        // the windows module's minimize and restore paths call this: a
        // background minimize (a taskbar's, an X11 iconify while unfocused)
        // emits nothing the listeners below hear, and the muted chip is
        // part of the frame
        Windows::Tasklist::taskChangedHook() = []() { barChanged(); };

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
            titleChanged();
        });
        supervisor().listen(EV.window.urgent, [](PHLWINDOW) { barChanged(); });
        // the tasklist's ⌃ and ✈ markers; one coalesced event after the layout settled
        supervisor().listen(EV.window.updateState, [](PHLWINDOW, const Desktop::View::SWindowStateUpdate&) { barChanged(); });
        supervisor().listen(EV.window.class_, [](PHLWINDOW) { barChanged(); });     // the task icon re-resolves
        supervisor().listen(EV.window.fullscreen, [](PHLWINDOW) { barChanged(); });
        supervisor().listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { barChanged(); });
        supervisor().listen(EV.workspace.active, [](PHLWORKSPACE) { barChanged(); });
        supervisor().listen(EV.workspace.created, [](PHLWORKSPACEREF) { barChanged(); });
        supervisor().listen(EV.workspace.removed, [](PHLWORKSPACEREF) { barChanged(); });
        supervisor().listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { barChanged(); });
        supervisor().listen(EV.monitor.layoutChanged, []() { barChanged(); });
        // An output going away (unplugged, or disabled with the lid shut)
        // takes what was open on it: a menu or a prompt drawn nowhere would
        // keep its bus session and swallow the next press or every key. Its
        // per-monitor hit cells go too.
        supervisor().listen(EV.monitor.removed, [](PHLMONITOR mon) {
            if (!mon)
                return;
            if (Menu::isOpen && Menu::mon.lock() == mon)
                Menu::close();
            if (Menubar::isOpen && Menubar::mon.lock() == mon)
                Menubar::close();
            forgetMonitor(mon->m_id);
            barChanged();
        });

        // Colors and fonts heal themselves — they are part of every texture's
        // cache key, so a changed value simply misses and rebuilds. What
        // doesn't is anything resolved from DISK at init: the icon dirs
        // (probed once from the GTK theme name) and the files found in them.
        // Desktop discovery is worker-owned and independent of the theme, so
        // a config reload does not restart it.
        supervisor().listen(EV.config.reloaded, []() {
            if (iconsReload())
                for (const auto& I : Tray::items)
                    I->dirty = true; // their textures came out of the theme we just dropped
            barChanged();
        });

        if (minute.start(onMinute))
            armMinute();

        HyprlandAPI::addLuaFunction(supervisor().handle(), "awesome", "menubar", luaMenubar);

        damageBars();
    }

    void CModule::teardown() {
        // reverse of init; the supervisor has already dropped the listeners
        // and every hop, so the units below only unwind their own state
        minute.stop();
        Windows::Tasklist::taskChangedHook() = nullptr;
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
        // an open menu sits over the prompt: Esc closes the innermost first
        if (Menu::onKey(e, info))
            return;
        Menubar::onKey(e, info);
    }
    void CModule::onInputBlocked() {
        Shell::onInputBlocked();
    }

    // `hyprctl awesome cells` (the gate's build): the first bar's click
    // cells, left to right, one "<widget> <x0> <x1>" a line — the gate's
    // map of what a press hits
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
#ifndef AWESOME_GATE
        (void)verb;
        return std::nullopt;
#else
        if (verb != "cells")
            return std::nullopt;
        const auto IT = hitboxes.begin();
        if (IT == hitboxes.end())
            return std::string{};
        const auto NAME = [](const IWidget* w) {
            return w == &taglistWidget() ? "tag" :
                w == &tasklistWidget()   ? "task" :
                w == &trayWidget()       ? "tray" :
                w == &batteryWidget()    ? "battery" :
                w == &controlsWidget()   ? "controls" :
                w == &bellWidget()       ? "bell" :
                w == &clockWidget()      ? "clock" :
                                           "?";
        };
        auto cells = IT->second;
        std::ranges::sort(cells, {}, [](const SHit& h) { return h.box.x; });
        std::string out;
        for (const auto& H : cells)
            out += std::format("{} {} {}\n", NAME(H.widget), (int)std::lround(H.box.x), (int)std::lround(H.box.x + H.box.w));
        return out;
#endif
    }

} // namespace NAwesome::Shell
