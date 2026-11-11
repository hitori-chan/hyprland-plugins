// The windows module: the window-state machine (normal / minimized /
// maximized), the focus policy, spawn placement, edge snapping, and the
// snap indicator.
//
// The pipeline order inside the module: the max swallow, then the snap
// commit, then the click policy (place and the state machine react to
// window events, not input).
#include "windows.hpp"

#include "windows/click.hpp"
#include "windows/max.hpp"
#include "windows/place.hpp"
#include "windows/snap.hpp"
#include "windows/tile.hpp"

#include "core/activate.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

#include <lua.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <string_view>
#include <vector>

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
    // the Lua unminimize(address): a call may come from a bind, inside the
    // keyboard emission, so the restore runs on the next loop turn (and
    // never under the lockscreen); quiet, as the hyprctl verb's
    static CHopQueue<PHLWINDOWREF, 16> pendingUnminimize([](std::vector<PHLWINDOWREF>& batch) {
        if (sessionLocked())
            return;
        for (const auto& WR : batch)
            if (const auto W = WR.lock(); W && W->mapped() && Tasklist::isMinimized(W))
                Tasklist::restore(W, true);
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

    namespace {
        std::string addressOf(const PHLWINDOW& w) {
            return std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get())); // hyprctl clients' form
        }

        // the windows this plugin hides, the most recently minimized last
        std::vector<std::string> minimizedAddresses() {
            Tasklist::prune();
            std::vector<std::string> out;
            for (const auto& M : Tasklist::minStack())
                if (const auto W = M.w.lock(); W && W->mapped())
                    out.emplace_back(addressOf(W));
            return out;
        }

        // `unminimize <address>`'s target: the window, or why there is none
        // ("usage: ...", "locked", "not minimized", "no such window")
        std::expected<PHLWINDOW, std::string> unminimizeTarget(std::string_view arg) {
            if (arg.starts_with("address:"))
                arg.remove_prefix(8);
            if (arg.empty())
                return std::unexpected("usage: awesome unminimize <address>");
            if (sessionLocked())
                return std::unexpected("locked"); // never un-minimize under the lockscreen
            for (const auto& W : Desktop::windowState()->windows()) {
                if (!W || !W->mapped() || addressOf(W) != arg)
                    continue;
                if (!Tasklist::isMinimized(W))
                    return std::unexpected("not minimized");
                return W;
            }
            return std::unexpected("no such window");
        }

        // `unminimize <address>`: a script's restore of one window, which
        // does not move the user. Quiet (Tasklist::restore): shown where it
        // is, not raised, not focused, no workspace change. Run inline: an
        // IPC request is no input or focus emission (invariant 6), and the
        // caller gets the outcome.
        std::string unminimizeVerb(std::string_view arg) {
            const auto W = unminimizeTarget(arg);
            if (!W)
                return W.error();
            Tasklist::restore(*W, true);
            return Tasklist::isMinimized(*W) ? "failed" : "ok";
        }

        // hl.plugin.awesome.minimized(): the addresses, as an array
        int luaMinimized(lua_State* L) {
            const auto ADDRS = minimizedAddresses();
            lua_createtable(L, (int)ADDRS.size(), 0);
            for (size_t i = 0; i < ADDRS.size(); i++) {
                lua_pushstring(L, ADDRS[i].c_str());
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            }
            return 1;
        }

        // hl.plugin.awesome.unminimize(address): the verb's outcome strings;
        // `ok` once the restore is queued (pendingUnminimize)
        int luaUnminimize(lua_State* L) {
            const char* ARG = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "";
            const auto  W   = unminimizeTarget(ARG);
            std::string out = !W ? W.error() : pendingUnminimize.push(PHLWINDOWREF{*W}) ? "ok" : "failed";
            if (out.starts_with("usage:"))
                out = "usage: hl.plugin.awesome.unminimize(address)";
            lua_pushstring(L, out.c_str());
            return 1;
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
        // An activation request (a notification click, a browser's "switch to
        // tab", any xdg-activation) reaches a MINIMIZED window. Minimize is
        // the plugin's state, so CWindow::activate() routes asks on hidden
        // windows to the urgency mark and the restore is ours: awesome's
        // "c.minimized = false + focus" for the ask, deferred out of the
        // emission the compositor runs its own activation from. Gated like
        // the compositor's own focus_on_activate: with it off, activation
        // never steals focus and the chip's urgent tint is the whole answer.
        supervisor().listen(Event::bus()->m_events.window.urgent, [](PHLWINDOW w) {
            // X11 asks (_NET_ACTIVE_WINDOW, DEMANDS_ATTENTION) are urgency-only
            // in both modes (docs/windows.md): Wine sends them on every internal
            // SetForegroundWindow, so they never restore. The per-window rule
            // gates like the compositor's own activate().
            static auto FOCUS_ON_ACTIVATE = CConfigValue<Config::INTEGER>("misc:focus_on_activate");
            if (!w || w->backend().isX11() || !w->m_ruleApplicator->focusOnActivate().valueOr(*FOCUS_ON_ACTIVATE) || !w->isHidden() || !Tasklist::isMinimized(w))
                return;
            // suppress_event activate|activatefocus keeps an ask urgency only,
            // as activate() does for a shown window (its private suppression
            // is read from these rules at map)
            if (std::ranges::any_of(w->m_ruleApplicator->static_.suppressEvent, [](const std::string& E) { return E == "activate" || E == "activatefocus"; }))
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
        HyprlandAPI::addLuaFunction(H, "awesome", "minimized", luaMinimized);
        HyprlandAPI::addLuaFunction(H, "awesome", "unminimize", luaUnminimize);
    }

    void CModule::teardown() {
        // reverse of init: the policies, then the state machine (which also
        // clears its per-window self-minimize listeners — a stateChanged
        // firing mid-teardown cannot re-arm a hop: the hops are already
        // reset by the supervisor)
        pendingActivates.reset();
        pendingFocusAway.reset();
        pendingRestoreLast.reset();
        pendingUnminimize.reset();
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

    // `hyprctl awesome minimized`: the windows this plugin hides, one
    // address a line, the most recently minimized last; `unminimize
    // <address>` (above). The gate's build adds `snap-zone`: the armed snap
    // zone, "<top|bottom|-> <left|right|->", "" with none.
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
        if (verb == "minimized") {
            std::string out;
            for (const auto& A : minimizedAddresses())
                out += A + "\n";
            return out;
        }
        if (verb == "unminimize" || verb.starts_with("unminimize "))
            return unminimizeVerb(verb == "unminimize" ? std::string_view{} : std::string_view{verb}.substr(11));
#ifdef AWESOME_GATE
        if (verb == "snap-zone")
            return Snap::zoneDescribe();
#endif
        return std::nullopt;
    }

    void CModule::onInputBlocked() {
        Max::onInputBlocked();
        Click::onInputBlocked();
        Snap::reset();
    }

} // namespace NAwesome::Windows
