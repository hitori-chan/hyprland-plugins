// awesome/windows/state.hpp — awesome's client.minimized, which the
// compositor has no flag for. The window-state machine: normal /
// minimized (with the held fullscreen/maximize mode), plus the
// self-minimize request routing (X11 WM_CHANGE_STATE, the CSD xdg
// set_minimize button, which Hyprland's onUpdateState ignores).
//
// The bar keeps only the VIEWS and the click routing; the STATE lives
// here, in the windows module, next to the maximize state it composes
// with.
#pragma once

#include "geometry.hpp"

#include "core/arrival.hpp"
#include "core/hop.hpp"
#include "core/queries.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NAwesome::Windows {

    // Every geometry the module sets goes through here: a target can outlive
    // its layout space (a swallowed window keeps its target, its slot is
    // removed), and the layout dereferences the space unchecked — a reflow
    // of such a window segfaulted the compositor.
    inline bool setGeom(const PHLWINDOW& w, const CBox& box) {
        const auto T = w ? w->windowTarget() : nullptr;
        if (!T || !T->space() || !g_layoutManager)
            return false;
        g_layoutManager->setTargetGeom(box, T);
        return true;
    }

} // namespace NAwesome::Windows

namespace NAwesome::Windows::Tasklist {

    // The shell repaints its tasklist when the minimized set changes: the
    // shell registers the hook at init (it registers before the windows
    // module, whose listeners cannot fire before its init returns).
    inline std::function<void()>& taskChangedHook() {
        static std::function<void()> H;
        return H;
    }

    // The window's raw pointer is the identity key (like the arrival order),
    // dropped in forget() on destroy before the pointer can be reused. `tiled`
    // records whether restore must re-add a layout slot: a floating window
    // reserves none, so hiding alone suffices and its box stays untouched —
    // routing it through the layout would risk the float-recenter (see max).
    // `fs` is the compositor fullscreen/maximize mode held at minimize: a
    // hidden window can't keep the workspace's one FS slot without stranding
    // it, so the mode is dropped before hiding and re-entered on restore —
    // awesome keeps a minimized client's fullscreen flag, this reproduces it.
    // The windows module's told-maximize is a plain floating window (internal
    // FSMODE_NONE), holds no slot, and passes through here untouched.
    struct SMinimized {
        PHLWINDOWREF                w;
        const void*                 key   = nullptr;
        bool                        tiled = false;
        Fullscreen::SFullscreenMode fs{};
    };
    inline std::vector<SMinimized>& minStack() {
        static std::vector<SMinimized> S; // most-recently minimized last
        return S;
    }

    // After hiding a window, commit focus to the next task. Two compositor
    // quirks shape this: setHidden's own focus reset only fires for the
    // swallow path (the swallowee is never focused), so minimizing the
    // FOCUSED window leaves focus on it; and a plain fullWindowFocus won't
    // move focus off a just-hidden window here. refocus() at the
    // successor's own center is the authoritative input path and lands right
    // regardless of where the real cursor sits (a click leaves it on the
    // bar). m_windows is bottom-first, so walk it reversed to pick the
    // topmost task.
    inline void focusNextAfterMinimize(const PHLWINDOW& gone, const PHLWORKSPACE& ws) {
        if (!ws || !g_pInputManager)
            return;
        const auto& WINS = Desktop::windowState()->windows();
        for (size_t i = WINS.size(); i-- > 0;) {
            const auto& W = WINS[i];
            if (W == gone || !W || !W->mapped() || W->isHidden() || !W->m_workspace || W->m_workspace->id() != ws->id())
                continue;
            g_pInputManager->refocus(W->middle());
            return;
        }
        // nothing left on the workspace: clear focus so it isn't stranded on
        // the hidden window.
        if (Desktop::focusState())
            Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW);
    }

    // Hyprland's onUpdateState ignores requestsMinimize, so a client's own
    // minimize button is dead without this — on BOTH backends: the xdg
    // set_minimize (CSD) and X11's _NET_WM_STATE_HIDDEN / WM_CHANGE_STATE
    // (XWayland clients, Wine). Both backends route the request into the
    // backend's stateRequest with a VOLATILE .minimized (the compositor
    // resets the flag right after the emit), so it's read synchronously in
    // the signal and the state change is deferred out of the request.
    inline std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener>& minReqListeners() {
        static std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener> M;
        return M;
    }

    inline bool isMinimized(const PHLWINDOW& w) {
        if (!w)
            return false;
        for (const auto& M : minStack())
            if (M.key == w.get())
                return true;
        return false;
    }

    inline void minimize(const PHLWINDOW& w) {
        if (!w || !w->mapped() || w->isHidden())
            return; // already hidden — minimized, or swallowed
        // Only the focused window pulls focus to a neighbor on hide; an app
        // minimizing a BACKGROUND window (its own set_minimized) must not
        // yank focus from wherever it currently sits.
        const bool WASFOCUSED = Desktop::focusState() && Desktop::focusState()->window() == w;
        // Drop the workspace's single FS slot before hiding: a fullscreen
        // or compositor-maximized window that goes hidden strands the slot
        // (black workspace, bar gone). Remember the mode; restore()
        // re-enters it. The told-maximize reports internal FSMODE_NONE and is
        // left alone — its box and told-state survive the hide untouched.
        const auto FS = Fullscreen::controller()->getFullscreenModes(w);
        if (FS.internal != Fullscreen::FSMODE_NONE)
            Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        const bool TILED = !w->isFloating();
        const auto WS    = w->m_workspace;
        if (TILED && g_layoutManager)
            g_layoutManager->removeTarget(w->layoutTarget());
        w->setHidden(true); // unrendered, xdg-suspended, no frame callbacks
        // setHidden issues NO damage. A tiled window's removeTarget reflow
        // repaints the vacated area for free, but a floating window leaves
        // its last frame stale on screen until something else damages it —
        // so its box flickers back under the cursor's motion damage. Force
        // the vacated area to repaint once here.
        if (g_pHyprRenderer)
            g_pHyprRenderer->damageWindow(w, true);
        if (WASFOCUSED)
            focusNextAfterMinimize(w, WS);
        minStack().push_back({PHLWINDOWREF{w}, w.get(), TILED, FS});
        if (taskChangedHook())
            taskChangedHook()();
    }

    // awesome's check_focus (awful.permissions): focus must never rest on a
    // minimized — invisible — window. In X11 a minimized client is unmapped
    // and simply can't take focus; Hyprland's focus fallback (e.g. closing
    // the last visible window) doesn't know a hidden window is "minimized"
    // and lands focus on it. Bounce it to the most-recent visible window on
    // the workspace (or clear focus), like awful.focus.history.get skipping
    // minimized. Called deferred from the window.active hook.
    inline void focusAwayFromHidden(const PHLWINDOW& w) {
        if (!w || !w->isHidden() || !isMinimized(w) || !w->m_workspace)
            return;
        if (Desktop::focusState() && Desktop::focusState()->window() == w)
            focusNextAfterMinimize(w, w->m_workspace);
    }

    // Raise, then focus with the window's REAL surface — not
    // Actions::focus(): that goes through FocusState with surface=nullptr,
    // and its already-focused guard compares (window, surface) ==
    // (m_focusWindow, m_focusSurface). When a popup/layer that held the
    // keyboard dies while the pointer sits on the bar (moves swallowed =
    // FFM can't heal), m_focusSurface is left empty with m_focusWindow
    // still set — nullptr == empty matches, the guard returns before the
    // raise AND before keyboard focus, and the click looks dead until some
    // other window gets focused. With the real surface the guard can never
    // match a half-focused window, and a focused-but-obscured one still
    // raises.
    inline void raiseAndFocus(const PHLWINDOW& w) {
        Desktop::windowState()->raise(w);
        if (Desktop::focusState())
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, w->wlSurface()->resource());
    }

    inline void restore(const PHLWINDOW& w) {
        if (!w)
            return;
        bool                        tiled = false, found = false;
        Fullscreen::SFullscreenMode fs{};
        auto& S                                                              = minStack();
        for (auto it = S.begin(); it != S.end(); ++it)
            if (it->key == w.get()) {
                tiled = it->tiled;
                fs    = it->fs;
                S.erase(it);
                found = true;
                break;
            }
        if (!found || !w->mapped())
            return;
        w->setHidden(false);
        if (tiled && g_layoutManager && w->m_workspace)
            g_layoutManager->newTarget(w->layoutTarget(), w->m_workspace->space());
        raiseAndFocus(w);
        // re-enter the fullscreen/maximize the window held when minimized,
        // after focus — the compositor fullscreens the active window.
        if (fs.internal != Fullscreen::FSMODE_NONE)
            Fullscreen::controller()->setFullscreenMode(w, fs.internal, fs.client);
        if (taskChangedHook())
            taskChangedHook()();
    }

    // Client minimize/unminimize requests, applied out of the request's
    // emission (one drain per turn, a burst coalesced; a client flooding
    // set_minimized is capped — 64 pending requests is far past any real
    // client's toggling).
    inline CHopQueue<std::pair<PHLWINDOWREF, bool>, 64>& minReqs() {
        static CHopQueue<std::pair<PHLWINDOWREF, bool>, 64> Q([](std::vector<std::pair<PHLWINDOWREF, bool>>& batch) {
            if (sessionLocked())
                return; // never hide/reorder windows under the lockscreen
            for (const auto& [WR, MIN] : batch) {
                const auto W = WR.lock();
                if (!W)
                    continue;
                if (MIN)
                    minimize(W);
                else
                    restore(W);
            }
        });
        return Q;
    }

    // Attach the self-minimize listener to a freshly-opened window (from
    // window.open). The listener is dropped in forget() on destroy / exit.
    // The BACKEND's stateRequest is the one signal both backends carry the
    // (volatile) minimize request on — the toplevel's raw stateChanged does
    // not exist for X11 windows at all.
    inline void watchMinimize(const PHLWINDOW& w) {
        if (!w)
            return;
        minReqListeners()[w.get()] = w->backend().m_events.stateRequest.listen([wr = PHLWINDOWREF{w}](const Desktop::View::SBackendStateRequest& req) {
            const auto W = wr.lock();
            if (!W)
                return;
            if (!req.minimized.has_value())
                return; // this request carried a fullscreen/maximize change, not a minimize
            minReqs().push({wr, *req.minimized});
        });
    }

    inline void minimizeFocused() {
        if (const auto W = Desktop::focusState() ? Desktop::focusState()->window() : nullptr)
            minimize(W);
    }

    inline void restoreLast() {
        // awful.client.restore: the most-recently minimized window whose tag
        // is currently viewed (isVisible), so it returns where you are.
        auto& S = minStack();
        for (size_t i = S.size(); i-- > 0;) {
            const auto W = S[i].w.lock();
            if (W && W->mapped() && W->m_workspace && W->m_workspace->visible()) {
                restore(W);
                return;
            }
        }
    }

    inline void forget(const void* w) {
        winOrder().forget(w);
        minReqListeners().erase(w);
        auto& S = minStack();
        std::erase_if(S, [w](const SMinimized& m) { return m.key == w || m.w.expired(); });
    }

    inline void exit() {
        winOrder().clear();
        minStack().clear();
        minReqs().reset();
        for (auto& [K, L] : minReqListeners())
            L.reset();
        minReqListeners().clear();
    }

} // namespace NAwesome::Windows::Tasklist
