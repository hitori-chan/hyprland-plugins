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

namespace NAwesome::Windows {

    CModule& module() {
        static CModule M;
        return M;
    }

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
