// awesome/windows.cpp — the windows module: the window-state machine
// (normal / minimized / maximized), the focus policy, spawn placement,
// edge snapping, and the snap indicator.
//
// The pipeline order inside the module is the old load order of the four
// policy plugins: the max swallow, then the snap commit, then the click
// policy. (place and the state machine react to window events, not input.)
#include "windows.hpp"

#include "windows/click.hpp"
#include "windows/max.hpp"
#include "windows/place.hpp"
#include "windows/snap.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

namespace NAwesome::Windows {

    CModule& module() {
        static CModule M;
        return M;
    }

    // the activate-of-a-minimized restore hop (init's window.urgent listener);
    // reset in teardown like every module hop
    static CHop pendingActivate;

    namespace {
        int luaMaximize(lua_State*) {
            Max::requestToggle();
            return 0;
        }
        int luaMinimize(lua_State*) {
            Tasklist::minimizeFocused();
            return 0;
        }
        int luaRestore(lua_State*) {
            Tasklist::restoreLast();
            return 0;
        }
        int luaFocusNext(lua_State*) {
            Click::focusByIdx(true);
            return 0;
        }
        int luaFocusPrev(lua_State*) {
            Click::focusByIdx(false);
            return 0;
        }
        int luaFocusPrevHere(lua_State*) {
            Click::focusPrevHere();
            return 0;
        }
    }

    void CModule::init() {
        // the state machine first: its window.open/close/destroy listeners
        // define the bookkeeping the views and policies read
        supervisor().listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) {
            if (w)
                Tasklist::watchMinimize(w);
        });
        supervisor().listen(Event::bus()->m_events.window.active, [](PHLWINDOW w, Desktop::eFocusReason reason) {
            Tasklist::focusAwayFromHidden(w);
        });
        supervisor().listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) {
            if (const auto* W = wr.get())
                Tasklist::forget(W);
        });
        // An activation request — a notification click, a browser's "switch to
        // tab", any xdg-activation — reaches a MINIMIZED window and dies
        // there. CWindow::activate() raises and focuses, but the window is
        // setHidden and activate() has no idea how to un-hide it: minimize
        // is OUR invention (the compositor has no such state), so the
        // restore is ours too. Without this the focus lands on an unrendered
        // window and the check_focus guard above bounces straight back off it
        // — the click does nothing at all.
        //
        // urgent fires from activate() BEFORE its focus_on_activate gate, so
        // read the same value the compositor is about to read: with it off
        // the user has asked that activation never steal focus, and
        // un-minimizing a window is exactly that theft — the chip's urgent
        // tint is the whole answer then.
        // Deferred like every other restore path; we are inside the emission
        // whose caller is about to run the compositor's own focus.
        supervisor().listen(Event::bus()->m_events.window.urgent, [](PHLWINDOW w) {
            static auto FOCUS_ON_ACTIVATE = CConfigValue<Config::INTEGER>("misc:focus_on_activate");
            if (!w || !*FOCUS_ON_ACTIVATE || !w->isHidden() || !Tasklist::isMinimized(w))
                return;
            PHLWINDOWREF WR{w};
            pendingActivate.arm([WR]() {
                if (const auto W = WR.lock(); W && W->mapped() && Tasklist::isMinimized(W))
                    Tasklist::restore(W); // un-hides, re-slots if tiled, raises and focuses
            });
        });

        Max::init();
        Click::init();
        Place::init();
        Snap::init();

        HANDLE H = supervisor().handle();
        HyprlandAPI::addLuaFunction(H, "awesome", "maximize", luaMaximize);
        HyprlandAPI::addLuaFunction(H, "awesome", "minimize", luaMinimize);
        HyprlandAPI::addLuaFunction(H, "awesome", "restore", luaRestore);
        HyprlandAPI::addLuaFunction(H, "awesome", "focus_next", luaFocusNext);
        HyprlandAPI::addLuaFunction(H, "awesome", "focus_prev", luaFocusPrev);
        HyprlandAPI::addLuaFunction(H, "awesome", "focus_prev_here", luaFocusPrevHere);
    }

    void CModule::teardown() {
        // reverse of init: the policies, then the state machine (which also
        // clears its per-window self-minimize listeners — a stateChanged
        // firing mid-teardown cannot re-arm a hop: the hops are already
        // reset by the supervisor)
        pendingActivate.reset();
        Snap::teardown();
        Place::teardown();
        Click::teardown();
        Max::teardown();
        Tasklist::exit();
    }

    // ---- the pipeline (max swallow -> snap commit -> click policy) ----

    void CModule::onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        Max::onPointerButton(e, info);
        if (info.cancelled)
            return;
        Snap::onInputEndingDrag();
        Click::onPointerButton(e, info);
    }

    void CModule::onPointerMove(const Vector2D&, Event::SCallbackInfo&) {
        Snap::onMouseMove();
    }

    void CModule::onKey(const IKeyboard::SKeyEvent&, Event::SCallbackInfo&) {
        Snap::onInputEndingDrag();
    }

    void CModule::onInputBlocked() {
        Max::onInputBlocked();
        Click::onInputBlocked();
        Snap::reset();
    }

} // namespace NAwesome::Windows
