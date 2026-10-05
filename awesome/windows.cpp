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

#include "core/activate.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

namespace NAwesome::Windows {

    CModule& module() {
        static CModule M;
        return M;
    }

    // the activate-of-a-minimized restore hop (init's window.urgent listener)
    // and the focus bounce off a minimized window (window.active); reset in
    // teardown like every module hop
    static CHop pendingActivate;
    static CHop pendingFocusAway;

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
            if (!w)
                return;
            Tasklist::watchMinimize(w);
        });
        // Focus landed on a minimized (hidden) window — the compositor's
        // fallback does not know our minimize. Bounce it, deferred: a focus
        // change inside the focus emission nests a second rawWindowFocus and
        // later listeners see the stale one (invariant 6).
        supervisor().listen(Event::bus()->m_events.window.active, [](PHLWINDOW w, Desktop::eFocusReason) {
            if (!w || !w->isHidden() || !Tasklist::isMinimized(w))
                return;
            pendingFocusAway.arm([WR = PHLWINDOWREF{w}]() {
                if (const auto W = WR.lock())
                    Tasklist::focusAwayFromHidden(W);
            });
        });
        supervisor().listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) {
            if (const auto* W = wr.get())
                Tasklist::forget(W);
        });
        // An activation request — a notification click, a browser's "switch to
        // tab", any xdg-activation — reaches a MINIMIZED window. Minimize is
        // OUR state (the compositor has no such thing), so CWindow::activate()
        // routes asks on hidden windows to the urgency mark, and the restore
        // is ours too: this is awesome's "c.minimized = false + focus" for
        // the ask (permissions.activate's raise hint), performed from the
        // event instead of inside the compositor's activation call.
        // Gated like the compositor's own focus_on_activate: with it off the
        // user has asked that activation never steal focus, and un-minimizing
        // a window is exactly that theft — the chip's urgent tint is the
        // whole answer then.
        // Deferred like every other restore path; we are inside the emission
        // the compositor is running its own activation from.
        supervisor().listen(Event::bus()->m_events.window.urgent, [](PHLWINDOW w) {
            // X11 asks (_NET_ACTIVE_WINDOW, DEMANDS_ATTENTION) are urgency-only
            // in both modes (README): Wine sends them on every internal
            // SetForegroundWindow, so they never restore. The per-window rule
            // gates like the compositor's own activate().
            static auto FOCUS_ON_ACTIVATE = CConfigValue<Config::INTEGER>("misc:focus_on_activate");
            if (!w || w->backend().isX11() || !w->m_ruleApplicator->focusOnActivate().valueOr(*FOCUS_ON_ACTIVATE) || !w->isHidden() || !Tasklist::isMinimized(w))
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

        // A user activation (tray icon, notification action) of an app whose
        // only window is minimized restores it: the click is the activation,
        // like awesome's c.minimized = false + focus. Core resolves the app
        // and asks through this seam.
        restoreMinimizedHook() = [](const PHLWINDOW& w) {
            if (!w || !w->mapped() || !Tasklist::isMinimized(w))
                return false;
            Tasklist::restore(w); // un-hides, re-slots if tiled, raises and focuses
            return true;
        };

        // Windows mapped before the plugin loaded (a runtime hyprpm reload)
        // get what window.open would have given them: the minimize and
        // client-unmaximize watches and an arrival number (their current Z
        // order — the best order left). Placement is not redone.
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->mapped())
                continue;
            Tasklist::watchMinimize(W);
            watchClientUnmax(W);
            winOrder().seqOf(W.get());
        }

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
        pendingFocusAway.reset();
        restoreMinimizedHook() = nullptr;
        Snap::teardown();
        Place::teardown();
        Click::teardown();
        Max::teardown();
        Tasklist::exit();
    }

    void CModule::persist() {
        Place::rememberOpen();
    }

    // ---- the pipeline (max swallow -> snap commit -> click policy) ----

    void CModule::onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        Max::onPointerButton(e, info);
        if (info.cancelled)
            return;
        Snap::onInputEndingDrag();
        Click::onPointerButton(e, info);
    }

    void CModule::onPointerMove(const Vector2D&, Event::SCallbackInfo& info) {
        if (info.cancelled) // an earlier module owns this point
            return;
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
