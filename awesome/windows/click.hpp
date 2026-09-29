// awesome/windows/click.hpp — awesome's click and focus-raise policy.
//
// 1. Click-to-raise: a plain left click brings the clicked window to the
//    top — over a maximized window too; clicking a fullscreen/maximized
//    window tucks the floaters back behind it. (Focus itself is native
//    follow_mouse. The compositor raises the seat's pointer-focus window
//    on every press; the raise here resolves a FRESH hit test and runs
//    after it, so a stale pointer focus — a map or unmap under a still
//    cursor — gets corrected, not compounded.)
// 2. Keyboard focus raises, hover focus doesn't (awesome's rule): focus
//    changes from binds and dispatchers bring the window to the top;
//    sloppy focus never does.
// 3. focus_prev_here — awesome's Mod+Tab: focus the previously focused
//    window ON THE CURRENT WORKSPACE (the native focus({ last }) follows
//    global history across workspaces).
// 4. focus_next()/focus_prev() — awesome's Mod+J/K (focus.byidx): cycle
//    the workspace's windows in arrival order — the native cycle walks the
//    z-order, which rule 2's raises rotate.
// 5. A click gesture aimed at a window that died under it never retargets:
//    the tail of a fast double-click on a click-to-close surface (an image
//    viewer's backdrop) is swallowed instead of focusing and raising
//    whatever sat beneath.
//
// The pipeline puts this AFTER the max swallow (the old load-order edge):
// a cancelled press never raises.
#pragma once

#include "state.hpp"

#include "core/queries.hpp"

#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NAwesome::Windows {

    namespace Click {
        inline void raiseWindow(PHLWINDOW w) {
            if (Fullscreen::controller()->isFullscreen(w)) {
                // "raising" the fullscreen/maximized window = tucking the
                // floaters back behind it. Clear ONLY the allowed-over flag
                // (under a fullscreen window the compositor shows floaters
                // by flag, not stack position) — never lower(): the re-stack
                // outlives a transient fullscreen viewer, and the chat
                // window it was opened from ended buried under every
                // maximized window on the workspace (the flag appears
                // mid-viewer through the compositor's pointer-focus raise on
                // press; reproduced live). Pinned windows stay above
                // fullscreen by design, like awesome's ontop.
                for (const auto& OW : Desktop::windowState()->windows()) {
                    if (OW == w || !OW->mapped() || OW->m_workspace != w->m_workspace || !OW->isAllowedOverFullscreen() || isPinned(OW))
                        continue;
                    OW->fullscreenPolicy().setAllowedOverFullscreen(false);
                    OW->updateFullscreenInputState();
                    *OW->presentation().alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN) = OW->isBlockedByFullscreen() ? 0.F : 1.F;
                }
            } else if (w->isFloating())
                Desktop::windowState()->raise(w);
        }
    }

    namespace {
        // queue+drain, never a lone doLaterLock: two raises/focuses can arm
        // in one dispatch (a press plus the focus it caused, a scripted bind
        // pair, an event backlog) and overwriting the lock cancels the
        // unfired one
        struct SRaiseJob {
            PHLWINDOWREF WR;
            bool         fullscreenOnly = false; // the press path: only fullscreen needs the allowed-over cleanup
        };
        inline std::vector<SRaiseJob>& raiseJobs() {
            static std::vector<SRaiseJob> J;
            return J;
        }
        inline std::vector<PHLWINDOWREF>& focusJobs() {
            static std::vector<PHLWINDOWREF> J;
            return J;
        }
        inline bool& raiseQueued() {
            static bool Q = false;
            return Q;
        }
        inline bool& focusQueued() {
            static bool Q = false;
            return Q;
        }
        inline CHop& pendingRaise() {
            static CHop H;
            return H;
        }
        inline CHop& pendingFocus() {
            static CHop H;
            return H;
        }

        inline void queueRaise(const PHLWINDOWREF& WR, bool fullscreenOnly) {
            if (raiseJobs().size() < 16)
                raiseJobs().push_back({WR, fullscreenOnly});
            if (raiseQueued())
                return;
            raiseQueued() = true;
            pendingRaise().arm([]() {
                raiseQueued() = false;
                const auto Q = std::move(raiseJobs());
                raiseJobs().clear();
                for (const auto& J : Q) {
                    if (sessionLocked())
                        return; // the lock can engage between the emission and this run
                    const auto W = J.WR.lock();
                    if (!W || !W->mapped())
                        continue;
                    if (J.fullscreenOnly && !Fullscreen::controller()->isFullscreen(W))
                        continue;
                    Click::raiseWindow(W);
                }
            });
        }

        inline void queueFocus(const PHLWINDOWREF& TARGET) {
            if (focusJobs().size() < 16)
                focusJobs().push_back(TARGET);
            if (focusQueued())
                return;
            focusQueued() = true;
            pendingFocus().arm([]() {
                focusQueued() = false;
                const auto Q = std::move(focusJobs());
                focusJobs().clear();
                for (const auto& TARGET : Q) {
                    if (sessionLocked())
                        return; // the lock can engage between the emission and this run
                    if (const auto W = TARGET.lock(); W && W->mapped())
                        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
                }
            });
        }

        // Corpse-guard gesture timing: a close this soon after a press on
        // the window reads as click-to-close, and a re-press this soon after
        // that close is the tail of the same double-click gesture (Qt and
        // GTK both default the double-click interval to 400ms).
        inline constexpr auto CLICK_KILL = std::chrono::milliseconds(500);
        inline constexpr auto GESTURE    = std::chrono::milliseconds(400);

        inline PHLWINDOWREF& pressWindow() {
            static PHLWINDOWREF W; // who took the last press, and when
            return W;
        }
        inline std::chrono::steady_clock::time_point& pressAt() {
            static std::chrono::steady_clock::time_point T{};
            return T;
        }
        inline uint32_t& swallowRelease() {
            static uint32_t R = 0;
            return R;
        }
        inline CBox& corpseBox() {
            static CBox B{}; // where the window the press killed last stood
            return B;
        }
        inline PHLWINDOWREF& corpseOwner() {
            static PHLWINDOWREF W; // while it lives, presses resolving to it pass
            return W;
        }
        inline uint32_t& corpseWs() {
            static uint32_t W = 0; // numbered workspace of the corpse, 0 = none (IDs start at 1)
            return W;
        }
        inline std::chrono::steady_clock::time_point& corpseUntil() {
            static std::chrono::steady_clock::time_point T{};
            return T;
        }

        // Arm (or grow) the corpse over `box` if the press on `w` was recent
        // enough to have caused its state change. A second arming inside a
        // live gesture (fullscreen exit, then the unmap) must never shrink
        // the guarded area, so an active corpse unions instead of being
        // replaced.
        inline void armCorpse(PHLWINDOW w, const CBox& box) {
            const auto NOW = std::chrono::steady_clock::now();
            if (NOW - pressAt() > CLICK_KILL)
                return;
            if (NOW < corpseUntil()) {
                const double X = std::min(corpseBox().x, box.x), Y = std::min(corpseBox().y, box.y);
                corpseBox() = CBox{X, Y, std::max(corpseBox().x + corpseBox().w, box.x + box.w) - X, std::max(corpseBox().y + corpseBox().h, box.y + box.h) - Y};
            } else
                corpseBox() = box;
            corpseOwner() = w;
            corpseWs()    = w->m_workspace ? w->m_workspace->numberedID().value_or(0) : 0;
            corpseUntil() = NOW + GESTURE;
        }
    }

    namespace Click {
        inline void onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
            const uint32_t BIT = trackedPointerButtonBit(e.button);

            if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
                // the release of a press swallowed below: swallow it too, or
                // the window under the cursor gets a release it never saw
                // pressed
                if (swallowRelease() & BIT) {
                    swallowRelease() &= ~BIT;
                    info.cancelled = true;
                }
                return;
            }

            // Buttons outside the shell's policy pass through untouched.
            if (!BIT)
                return;

            // Already swallowed earlier in the pipeline — the shell cancels
            // clicks on its strip and open menus, the max swallow cancels
            // Super-grabs on maximized windows. Those are never raise clicks.
            if (info.cancelled)
                return;

            // The pointer is on a layer surface (a bar): that click is the
            // bar's, never reach through it to the window underneath.
            if (nativePointerGrabActive() || nativeLayerOwnsPointer())
                return;

            const auto NOW = std::chrono::steady_clock::now();
            const auto POS = g_pInputManager->getMouseCoordsInternal();
            const auto W   = windowUnderCursor();

            // The corpse guard: a press inside the box of the window the
            // previous press just killed is the tail of the same gesture — a
            // fast click burst on a click-to-close surface (an image
            // viewer's backdrop), aimed at a window that died faster than
            // the hand can abort. Left through, the compositor would focus
            // — and the raise below would lift — whatever sat beneath.
            // Cancelling here stops focus, raise and delivery at once: this
            // emission precedes all compositor handling. Each swallowed
            // press extends the guard (a burst is one gesture); a press
            // resolving to the corpse's still-living owner passes — that is
            // a click ON the window, not through where it used to be; and
            // the gesture's claim ends off the corpse's workspace, judged by
            // the window the press would land on (a special-workspace window
            // over the corpse box is not the corpse's).
            if (NOW < corpseUntil() && corpseBox().containsPoint(POS) && (!W || W != corpseOwner().lock())) {
                const auto MON          = monitorAt(POS);
                const bool ON_CORPSE_WS = W ? (W->m_workspace && W->m_workspace->numberedID().value_or(0) == corpseWs())
                                            : (MON && MON->m_activeWorkspace && MON->m_activeWorkspace->numberedID().value_or(0) == corpseWs());
                if (ON_CORPSE_WS) {
                    info.cancelled = true;
                    swallowRelease() |= BIT;
                    corpseUntil() = NOW + GESTURE;
                    return;
                }
            }

            // awesome's click-to-raise. A Super+left/right press raises too
            // — grabbing a window raised in awesome as well.
            if (e.button != BTN_LEFT && !(e.button == BTN_RIGHT && superHeld()))
                return;

            // Only a press that can invoke this plugin's raise policy can
            // cause the pressed window to arm corpse protection on its
            // close/fullscreen event. Middle and ordinary right clicks must
            // never leave a later press guarded as if they had been
            // click-to-close gestures.
            pressWindow() = W;
            pressAt()     = NOW;

            // Fullscreen needs the plugin's special allowed-over cleanup.
            // Ordinary floating windows are raised synchronously by
            // Hyprland's native hit-tested press path, so scheduling a
            // second raise only adds work and can reapply stack state after
            // another input event.
            if (W)
                queueRaise(PHLWINDOWREF{W}, true);
        }

        inline void onInputBlocked() {
            swallowRelease() = 0;
            corpseUntil()    = {};
            corpseOwner().reset();
            pressWindow().reset();
        }

        // Keyboard focus raises, hover focus doesn't. The raise is deferred
        // out of the focus emission (rawWindowFocus is still running when
        // this fires).
        inline void onWindowActive(PHLWINDOW w, Desktop::eFocusReason reason) {
            using enum Desktop::eFocusReason;
            if (!w ||
                (reason != FOCUS_REASON_KEYBIND && reason != FOCUS_REASON_DISPATCH_FOCUSWINDOW && reason != FOCUS_REASON_SWITCH_TO_WINDOW_SOFT &&
                 reason != FOCUS_REASON_SWITCH_TO_WINDOW_HARD))
                return;

            queueRaise(PHLWINDOWREF{w}, false);
        }

        // focus_prev_here — awesome's Mod+Tab: the most recently focused
        // OTHER window on the current workspace, focused + raised. Each focus
        // rewrites the history head, so repeated presses bounce between the
        // two most recent windows, like awful.client.focus.history.previous.
        inline void focusPrevHere() {
            const auto FOCUS = Desktop::focusState()->window();
            const auto MON   = Desktop::focusState()->monitor();
            const auto WS    = MON ? MON->m_activeWorkspace : nullptr;
            if (!WS)
                return;

            // the tracker is ordered old -> new: the previous window is at
            // the BACK
            const auto& HIST = Desktop::History::windowTracker()->fullHistory();
            for (auto it = HIST.rbegin(); it != HIST.rend(); ++it) {
                const auto W = it->lock();
                if (!W || W == FOCUS || !W->mapped() || W->isHidden() || W->m_workspace != WS)
                    continue;

                queueFocus(PHLWINDOWREF{W});
                break;
            }
        }

        // focus_next/focus_prev — awesome's Mod+J/K (focus.byidx): cycle the
        // workspace's windows in ARRIVAL order, wrapping. The native cycle
        // walks the z-order list, which raise-on-focus rotates every press:
        // forward still visits everything (the wrap always lands on the
        // lowest window), but backward reads the second-from-top and bounces
        // between the two newest raises.
        inline void focusByIdx(bool next) {
            const auto MON = Desktop::focusState()->monitor();
            const auto WS  = MON ? MON->m_activeWorkspace : nullptr;
            if (!WS)
                return;

            const auto WINS = winOrder().onWorkspace(WS);
            const auto NEXT = CArrivalOrder::step(WINS, Desktop::focusState()->window(), next ? 1 : -1);
            if (!NEXT)
                return;

            queueFocus(PHLWINDOWREF{NEXT});
        }

        inline void init() {
            supervisor().listen(Event::bus()->m_events.window.active, [](PHLWINDOW w, Desktop::eFocusReason reason) { onWindowActive(w, reason); });
            supervisor().listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) {
                if (w)
                    winOrder().seqOf(w.get()); // stamp arrival at map, not at first cycle
            });
            supervisor().listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) { winOrder().forget(wr.get()); });
            supervisor().listen(Event::bus()->m_events.window.close, [](PHLWINDOW w) {
                // the pressed window died right after the press:
                // click-to-close. Arm the corpse guard over where the user
                // last saw it (a fullscreen viewer's corpse is the whole
                // screen — this fires before the unmap resets fullscreen
                // state).
                if (!w || w != pressWindow().lock())
                    return;
                pressWindow().reset();
                armCorpse(w, w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
            });
            supervisor().listen(Event::bus()->m_events.window.fullscreen, [](PHLWINDOW w) {
                // the pressed window leaving fullscreen right after the
                // press is the click-to-close family too: some viewers exit
                // fullscreen before unmapping, and in that gap the restored
                // box no longer covers what the screen still shows. Guard the
                // monitor box it vacated; the owner lives, so presses
                // resolving to it still pass.
                if (!w || w != pressWindow().lock() || Fullscreen::controller()->isFullscreen(w))
                    return;
                if (const auto MON = w->m_monitor.lock())
                    armCorpse(w, MON->logicalBox());
            });
        }

        inline void teardown() {
            swallowRelease() = 0;
            corpseUntil()    = {};
            corpseOwner().reset();
            pressWindow().reset();
        }
    } // namespace Click

} // namespace NAwesome::Windows
