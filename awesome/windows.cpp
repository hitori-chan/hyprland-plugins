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
#include "windows/tile.hpp"

#include "core/activate.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

namespace NAwesome::Windows {

    CModule& module() {
        static CModule M;
        return M;
    }

    // the activate-of-a-minimized restores (init's window.urgent listener) —
    // queued, never a lone hop: two asks in one dispatch must both land —
    // and the focus bounce off a minimized window (window.active); reset in
    // teardown like every module hop
    static CHopQueue<PHLWINDOWREF, 16> pendingActivates([](std::vector<PHLWINDOWREF>& batch) {
        if (sessionLocked())
            return; // never un-minimize under the lockscreen
        for (const auto& WR : batch)
            if (const auto W = WR.lock(); W && W->mapped() && Tasklist::isMinimized(W))
                Tasklist::restore(W); // un-hides, re-slots if tiled, raises and focuses
    });
    static CHop pendingFocusAway;
    // the restore bind, out of the keybind's dispatch like every focus change
    // (invariant 6) and never under the lockscreen; a queue, not one hop:
    // two presses in one dispatch restore two windows
    static CHopQueue<char, 16> pendingRestoreLast([](std::vector<char>& batch) {
        for (size_t i = 0; i < batch.size() && !sessionLocked(); i++)
            Tasklist::restoreLast();
    });

    namespace {
        int luaMaximize(lua_State*) {
            Max::requestToggle();
            return 0;
        }
        // A bind runs inside the keyboard emission, under a lock too (a
        // bindl): the window is taken at the press, the hide and the focus
        // move run deferred, and the drains refuse under the lockscreen.
        int luaMinimize(lua_State*) {
            if (const auto W = Desktop::focusState() ? Desktop::focusState()->window() : nullptr)
                Tasklist::minReqs().push({PHLWINDOWREF{W}, true});
            return 0;
        }
        int luaRestore(lua_State*) {
            pendingRestoreLast.push(0);
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
            Tasklist::onMap(w);
            Tasklist::watchMinimize(w);
        });
        // Focus landed on a minimized (hidden) window. A switch to that
        // window by name (Mod+U on an urgent one, a focus dispatch) restores
        // it, as awesome's jump_to un-minimizes; the compositor's fallbacks
        // do not know our minimize, so they bounce. Deferred: a focus change
        // inside the focus emission nests a second rawWindowFocus and later
        // listeners see the stale one (invariant 6).
        supervisor().listen(Event::bus()->m_events.window.active, [](PHLWINDOW w, Desktop::eFocusReason reason) {
            using enum Desktop::eFocusReason;
            if (!w || !w->isHidden() || !Tasklist::isMinimized(w))
                return;
            if (reason == FOCUS_REASON_DISPATCH_FOCUSWINDOW || reason == FOCUS_REASON_SWITCH_TO_WINDOW_SOFT || reason == FOCUS_REASON_SWITCH_TO_WINDOW_HARD) {
                pendingActivates.push(PHLWINDOWREF{w});
                return;
            }
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
            // in both modes (docs/windows.md): Wine sends them on every internal
            // SetForegroundWindow, so they never restore. The per-window rule
            // gates like the compositor's own activate().
            static auto FOCUS_ON_ACTIVATE = CConfigValue<Config::INTEGER>("misc:focus_on_activate");
            if (!w || w->backend().isX11() || !w->m_ruleApplicator->focusOnActivate().valueOr(*FOCUS_ON_ACTIVATE) || !w->isHidden() || !Tasklist::isMinimized(w))
                return;
            // minimized on a workspace no monitor shows: an ask there is
            // urgency only in both modes, as for a shown window
            if (!w->m_workspace || !w->m_workspace->visible())
                return;
            pendingActivates.push(PHLWINDOWREF{w});
        });
        // A taskbar's minimize (wlr-foreign-toplevel set/unset_minimized):
        // the same state request a client's own minimize button makes
        supervisor().listen(Event::bus()->m_events.window.minimize, [](PHLWINDOW w, bool minimized) {
            if (w)
                Tasklist::minReqs().push({PHLWINDOWREF{w}, minimized});
        });

        Tile::init();
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
        // order — the best order left), and a plugin maximize the previous
        // instance left is adopted. Placement is not redone.
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->mapped())
                continue;
            Tasklist::watchMinimize(W);
            watchClientUnmax(W);
            adoptOnLoad(W);
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
        pendingActivates.reset();
        pendingFocusAway.reset();
        pendingRestoreLast.reset();
        restoreMinimizedHook() = nullptr;
        Snap::teardown();
        Place::teardown();
        Click::teardown();
        Max::teardown();
        Tile::teardown();
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

    // `hyprctl awesome snap-zone` (the gate's build): the armed snap zone,
    // "<top|bottom|-> <left|right|->", "" with none
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
#ifndef AWESOME_GATE
        (void)verb;
        return std::nullopt;
#else
        if (verb != "snap-zone")
            return std::nullopt;
        return Snap::zoneDescribe();
#endif
    }

    void CModule::onInputBlocked() {
        Max::onInputBlocked();
        Click::onInputBlocked();
        Snap::reset();
    }

} // namespace NAwesome::Windows
