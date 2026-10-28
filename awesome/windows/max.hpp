// awesome/windows/max.hpp — awesome's per-window maximize as a native
// plugin.
//
// awesome's Mod+M: maximized is a PER-WINDOW flag there, any number at
// once. The compositor's internal maximize enforces one per workspace
// (granting one steals the previous, and a later fullscreen window EVICTS
// a maximized holder), and sync_fullscreen mirrors a client-only mode
// back into that machinery — so this maximize never enters compositor
// fullscreen state at all: the client is told (xdg set_maximized) and the
// window is sized to the workarea, exactly awesome's model. Maximize the
// compositor granted on its own (initial-maximize at map, app requests)
// is ADOPTED into this model on sight, for the same reason.
//
// The last windowed box is remembered per app class across window closes
// AND relogs (the core's unified state file, core/state.hpp): un-maximizing
// a born-maximized window restores it instead of the client's guess (GTK
// forgets its normal geometry across restarts).
//
// Maximized windows are immovable, like awesome's: Super+left/right-click
// on one is swallowed whole — without this, the drag bind "picks the
// window up" at press time, instantly unmaximizing it and yoinking it to
// the cursor at its restored size. The pipeline puts this before the
// click policy, so the swallow wins over click-to-raise.
#pragma once

#include "geometry.hpp"
#include "state.hpp"

#include "core/persist.hpp"
#include "core/queries.hpp"
#include "core/state.hpp"

#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/desktop/view/window/WindowStateTracker.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace NAwesome::Windows {

    namespace {
        // plugin-maximized windows and their restore geometry, relative to
        // the window's monitor origin (a workspace moved to another monitor
        // restores there, not clamped against the old one's edge). An empty
        // box: no windowed geometry ever existed.
        inline std::unordered_map<PHLWINDOWREF, CBox>& maximized() {
            static std::unordered_map<PHLWINDOWREF, CBox> M;
            return M;
        }
        inline CBox monLocal(const CBox& global, const PHLMONITOR& mon) {
            if (!mon || global.w <= 5 || global.h <= 5)
                return global;
            return CBox{global}.translate(-mon->logicalBox().pos());
        }
        inline CBox monGlobal(const CBox& local, const PHLMONITOR& mon) {
            if (!mon || local.w <= 5 || local.h <= 5)
                return local;
            return CBox{local}.translate(mon->logicalBox().pos());
        }

        // last windowed box per app class, surviving window closes and
        // relogs: the restore target when a window of that app is born
        // maximized again. The store is the core's unified state
        // (core/state.hpp), relative to the monitor's origin.
        inline CRecentMap<Box>& lastWindowed() {
            return StateStore::inst().data().windowed;
        }

        // the app's remembered windowed box, on `mon`
        inline std::optional<CBox> windowedOn(const std::string& cls, PHLMONITOR mon) {
            const auto* B = lastWindowed().find(cls);
            if (!B || !mon)
                return std::nullopt;
            const auto O = mon->logicalBox().pos();
            return CBox{O.x + B->x, O.y + B->y, (double)B->w, (double)B->h};
        }

        inline CBox boundedRestore(PHLWINDOW w, const CBox& box, const CBox& workarea) {
            const auto T = w ? w->windowTarget() : nullptr;
            if (!T)
                return box;
            return Geometry::boundedRestore(box, workarea, T->minSize(), T->maxSize());
        }

        inline void loadWindowed() {
            // the state is loaded by the supervisor before module inits;
            // only a real windowed size is a restore target
            lastWindowed().eraseIf([](const std::string&, const Box& b) { return b.w <= 5 || b.h <= 5; });
        }

        inline void rememberWindowed(const std::string& cls, const CBox& box, PHLMONITOR mon) {
            if (cls.empty() || !mon || box.w <= 5 || box.h <= 5)
                return;
            const auto O = mon->logicalBox().pos();
            if (lastWindowed().put(cls, Box{(int)std::llround(box.x - O.x), (int)std::llround(box.y - O.y), (int)std::llround(box.w), (int)std::llround(box.h)}))
                StateStore::inst().dirty();
        }

        inline bool pluginMaximized(PHLWINDOW w) {
            return maximized().contains(PHLWINDOWREF{w});
        }

        // Compositor-granted maximize (born-maximized at map, app request)
        // holds the workspace's single internal fullscreen slot: a later
        // fullscreen window evicts it, and it never comes back. Dissolve
        // the grant into plugin maximize instead — slot freed, told-state
        // and workarea box kept.
        inline void adoptCompositorMax(PHLWINDOW W) {
            // a group lays out from the group's target: the member's box is
            // ignored (the toggle's own rule)
            if (!W || !W->mapped() || !W->windowTarget() || !W->isFloating() || W->grouping().group() || pluginMaximized(W))
                return;
            if (Fullscreen::controller()->getFullscreenModes(W).internal != Fullscreen::FSMODE_MAXIMIZED)
                return;
            const auto MON = W->m_monitor.lock();
            if (!MON)
                return;

            Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
            // the exit just granted the client the size choice — the box is ours
            W->cancelClientSize();
            // the geometry change below re-enters the floating recalc, which
            // would fire the one-shot respawnIfBornFullscreen and re-arm the
            // 0x0 client-size grant on top of the plugin box; a client
            // answering with its normal size would resize the
            // plugin-maximized window out from under us
            W->m_bornFullscreen = false;

            const auto WA = MON->logicalBoxMinusReserved();
            // empty restore box = no windowed geometry ever existed;
            // un-maximizing hands the size choice to the client
            CBox restore{};
            if (const auto B = windowedOn(W->metadata().appID(), MON))
                restore = monLocal(boundedRestore(W, *B, WA), MON);
            maximized()[PHLWINDOWREF{W}] = restore;

            if (auto TOP = xdgToplevel(W))
                TOP->setMaximized(true);
            // through the layout, never the raw target: the floating
            // algorithm's fullscreen-exit recenter restores ITS tracked
            // lastBox — a raw setPositionGlobal leaves that stale at the
            // pre-maximize box, and the window "un-maximizes" on any later
            // fullscreen roundtrip
            setGeom(W, WA);
            W->windowTarget()->warpPositionSize();
        }

        // The pointer follows the geometry, like the compositor's own
        // maximize/fullscreen dispatchers: a window that grows under a
        // still cursor (an adopted born-maximized window, a restore) never
        // got the pointer — and a press on the already-focused window does
        // not refocus it, so the first click went nowhere (2026-10-04: a
        // reopened maximized firefox ignored the titlebar double-click).
        // Called once per drain, from the event loop, never from an input
        // emission.
        inline void pointerFollowsGeometry() {
            if (g_pInputManager && !sessionLocked())
                g_pInputManager->simulateMouseMovement();
        }

        inline bool& reflowQueued() {
            static bool Q = false;
            return Q;
        }
        inline CHop& pendingReflow() {
            static CHop H;
            return H;
        }

        // A plugin-maximized window is a client-only state, so Hyprland's
        // layout reflow does not resize it when a workspace changes monitor
        // or a reserved area changes. Coalesce those events and apply the
        // current workarea after the compositor finishes its own movement.
        inline bool reflowMaximized() {
            bool moved = false;
            for (const auto& ENTRY : maximized()) {
                const auto W = ENTRY.first.lock();
                if (!W || !W->mapped() || W->isHidden() || !W->isFloating() || !W->windowTarget())
                    continue;
                const auto MODES = Fullscreen::controller()->getFullscreenModes(W);
                if (MODES.internal != Fullscreen::FSMODE_NONE || MODES.client != Fullscreen::FSMODE_NONE)
                    continue; // a native fullscreen/maximize grant owns the geometry
                const auto MON = W->m_monitor.lock();
                if (!MON)
                    continue;
                const auto WA = MON->logicalBoxMinusReserved();
                if (W->windowTarget()->position() == WA)
                    continue;
                setGeom(W, WA);
                W->windowTarget()->warpPositionSize();
                moved = true;
            }
            return moved;
        }

        inline void queueReflow() {
            if (reflowQueued())
                return;
            reflowQueued() = true;
            pendingReflow().arm([]() {
                reflowQueued() = false;
                if (reflowMaximized())
                    pointerFollowsGeometry();
            });
        }

        // queued, never a lone doLaterLock: two born-maximized windows can
        // map in one dispatch, and overwriting the lock cancels the unfired
        // one
        inline CHopQueue<PHLWINDOWREF, 64>& adopts() {
            static CHopQueue<PHLWINDOWREF, 64> Q([](std::vector<PHLWINDOWREF>& batch) {
                for (const auto& WR : batch)
                    adoptCompositorMax(WR.lock());
                pointerFollowsGeometry();
            });
            return Q;
        }
        inline void queueAdopt(PHLWINDOW w) {
            adopts().push(PHLWINDOWREF{w});
        }

        // plugin maximize is a floating state: an entry of a window that left
        // floating is on its way out (window.updateState), never immovable
        inline bool maximizedAny(PHLWINDOW w) {
            return (pluginMaximized(w) && w->isFloating()) || Fullscreen::controller()->isFullscreen(w);
        }

        // The maximize toggle (hl.plugin.awesome.maximize, awesome's Mod+M).
        inline void applyMaxToggle(const PHLWINDOWREF& WR) {
            const auto W = WR.lock();
            if (!W || !W->mapped() || !W->windowTarget())
                return;
            // the lock can engage between the keypress and this deferred run
            if (sessionLocked())
                return;

            std::erase_if(maximized(), [](const auto& E) { return E.first.expired(); });

            const auto FSMODE = Fullscreen::controller()->getFullscreenModes(W).internal;

            // awesome keeps maximized and fullscreen independent: the toggle
            // must never drop a genuinely-fullscreen window (nor
            // plugin-maximize one — its fullscreen box is not a windowed
            // size).
            if (FSMODE == Fullscreen::FSMODE_FULLSCREEN)
                return;

            // Compositor-maximized (born maximized, app request): native
            // unmax.
            if (FSMODE == Fullscreen::FSMODE_MAXIMIZED) {
                Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

                // Born maximized: the compositor just granted the client the
                // size choice (m_sizeFromClientSerial armed). If this app's
                // windowed box is remembered, that beats the client's answer
                // — GTK forgets its normal geometry across restarts.
                const auto MON = W->m_monitor.lock();
                const auto B   = windowedOn(W->metadata().appID(), MON);
                if (B && W->m_sizeFromClientSerial && W->isFloating()) {
                    W->cancelClientSize();
                    setGeom(W, boundedRestore(W, *B, MON->logicalBoxMinusReserved()));
                    W->windowTarget()->warpPositionSize();
                }
                return;
            }

            const auto MON = W->m_monitor.lock();
            if (!MON)
                return;

            // Tiled windows maximize natively: the plugin box is a floating
            // mechanism, and the native unmax above serves both layouts, so
            // a tiled re-max must go through the native path or the toggle
            // is one-directional (unmax works, re-max is a no-op). A plugin
            // entry that survived a re-tile is stale — the native mechanism
            // owns the geometry from here, and its box is the workarea, not
            // a windowed size.
            if (!W->isFloating()) {
                maximized().erase(WR);
                Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_MAXIMIZED, Fullscreen::FSMODE_MAXIMIZED);
                return;
            }

            // A floating group lays out from the group's own target: the
            // member's box is ignored, and the client would be told a
            // maximize nobody sees. (A plugin entry cannot exist for it:
            // the restore branch below still runs for one that predates
            // the grouping.)
            if (W->grouping().group() && !pluginMaximized(W))
                return;

            // X11 has no maximize hint on this path; geometry alone.
            const auto setClientMaximized = [&W](bool m) {
                if (auto TOP = xdgToplevel(W))
                    TOP->setMaximized(m);
            };

            const auto WA = MON->logicalBoxMinusReserved();

            if (const auto IT = maximized().find(WR); IT != maximized().end()) {
                const CBox STORED = monGlobal(IT->second, MON);
                maximized().erase(IT);
                // tell the client the truth: a client never told it left
                // maximized (notably GTK) stays in maximized mode — it
                // saves "maximized" on close and reopens maximized, and
                // stops tracking its normal geometry (2026-10-03: firefox
                // "always opens maximized"). Its windowed CSD shadow
                // margin is cropped at the box by the fork's renderer;
                // the fork no longer lies maximized at map to suppress it.
                setClientMaximized(false);
                if (STORED.w > 5 && STORED.h > 5) {
                    const CBox R = boundedRestore(W, STORED, WA);
                    rememberWindowed(W->metadata().appID(), R, W->m_monitor.lock());
                    setGeom(W, R);
                    W->windowTarget()->warpPositionSize();
                } else {
                    // adopted with no remembered box: the client picks its
                    // size (the 0x0 grant; the commit adoption recenters it)
                    W->requestClientSize();
                }
            } else {
                const auto BOX = W->windowTarget()->position();
                maximized().emplace(WR, monLocal(BOX, MON));
                rememberWindowed(W->metadata().appID(), BOX, W->m_monitor.lock());
                setClientMaximized(true);
                setGeom(W, WA);
                W->windowTarget()->warpPositionSize();
                Desktop::windowState()->raise(W);
            }
        }

        // A client's own unmaximize (its titlebar restore button, a titlebar
        // double-click) on a plugin-maximized window. The compositor drops
        // it — it only honors an unmaximize for a window IT holds maximized,
        // and the adoption cleared that — so the client stayed told
        // maximized forever, saved "maximized" on close and reopened
        // maximized (2026-10-04: firefox "always opens maximized"). The
        // request flag is volatile, so it is read in the backend's emission;
        // the restore is deferred out of it.
        inline std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener>& unmaxRequestListeners() {
            static std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener> M;
            return M;
        }
        inline CHopQueue<PHLWINDOWREF, 16>& clientUnmaxes() {
            static CHopQueue<PHLWINDOWREF, 16> Q([](std::vector<PHLWINDOWREF>& batch) {
                // still plugin-maximized: the toggle takes its restore
                // branch (a Mod+M in the same dispatch may have won)
                for (const auto& WR : batch)
                    if (const auto W = WR.lock(); W && pluginMaximized(W))
                        applyMaxToggle(WR);
                pointerFollowsGeometry();
            });
            return Q;
        }

        // A client's titlebar drag (an xdg move; XWayland advertises
        // _NET_WM_MOVERESIZE but never handles it) on a plugin-maximized
        // window: the compositor began the drag in the
        // same emission (its own listener runs first), and the Super-drag
        // swallow never saw it. awesome's mouse.client.move refuses a
        // maximized client: end the drag out of the emission (the end
        // refocuses) and re-assert the box.
        inline std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener>& moveRequestListeners() {
            static std::unordered_map<const void*, Hyprutils::Signal::CHyprSignalListener> M;
            return M;
        }
        inline CHopQueue<PHLWINDOWREF, 16>& refusedDrags() {
            static CHopQueue<PHLWINDOWREF, 16> Q([](std::vector<PHLWINDOWREF>& batch) {
                for (const auto& WR : batch) {
                    const auto W = WR.lock();
                    if (W && pluginMaximized(W) && g_layoutManager && g_layoutManager->dragController()->target() == W->layoutTarget())
                        g_layoutManager->endDragTarget();
                }
                queueReflow();
            });
            return Q;
        }

        inline void watchClientUnmax(const PHLWINDOW& w) {
            unmaxRequestListeners()[w.get()] = w->backend().m_events.stateRequest.listen([wr = PHLWINDOWREF{w}](const Desktop::View::SBackendStateRequest& req) {
                if (!req.maximized.has_value() || *req.maximized)
                    return;
                const auto W = wr.lock();
                if (W && pluginMaximized(W))
                    clientUnmaxes().push(wr);
            });
            moveRequestListeners()[w.get()] = w->backend().m_events.moveRequest.listen([wr = PHLWINDOWREF{w}]() {
                if (const auto W = wr.lock(); W && pluginMaximized(W))
                    refusedDrags().push(wr);
            });
        }

        // A plugin-maximized window outlives a reload (or disable/enable)
        // at the workarea box, told maximized, which the fresh plugin does
        // not know: the next Mod+M would "maximize" it again and save the
        // workarea as its windowed box, and its own unmaximize button would
        // be dropped. Adopt it, restoring to the app's remembered windowed
        // box (or the client's own size).
        inline void adoptOnLoad(const PHLWINDOW& w) {
            if (!w || !w->mapped() || !w->isFloating() || !w->windowTarget() || pluginMaximized(w))
                return;
            if (Fullscreen::controller()->getFullscreenModes(w).internal != Fullscreen::FSMODE_NONE)
                return; // a compositor mode: the open/commit adoption's case
            const auto MON = w->m_monitor.lock();
            if (!MON)
                return;
            const auto CUR = w->windowTarget()->position();
            const auto WA  = MON->logicalBoxMinusReserved();
            if (CUR.x > WA.x || CUR.y > WA.y || CUR.x + CUR.w < WA.x + WA.w || CUR.y + CUR.h < WA.y + WA.h)
                return;
            // an xdg client must have been TOLD maximized: a float that is
            // merely sized to the workarea keeps being one (X11 clients are
            // never told; the box is all there is)
            if (const auto TOP = xdgToplevel(w); TOP && std::ranges::find(TOP->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED) == TOP->m_pendingApply.states.end())
                return;
            CBox restore{};
            if (const auto B = windowedOn(w->metadata().appID(), MON))
                restore = monLocal(boundedRestore(w, *B, WA), MON);
            maximized()[PHLWINDOWREF{w}] = restore;
        }

        // A plugin-maximized window that leaves floating (togglefloating, a
        // re-tile) or unmaps no longer owns the plugin box: drop the entry,
        // keep the windowed box it held as the app's remembered one, and
        // tell the client it left maximize. Left in place, the entry made a
        // tiled window immovable, turned its CSD unmaximize into a native
        // maximize and snapped it back to the workarea once floated again.
        inline void dropMaximize(const PHLWINDOW& w) {
            const auto IT = maximized().find(PHLWINDOWREF{w});
            if (IT == maximized().end())
                return;
            const auto MON = w->m_monitor.lock();
            rememberWindowed(w->metadata().appID(), monGlobal(IT->second, MON), MON);
            maximized().erase(IT);
            if (auto TOP = xdgToplevel(w))
                TOP->setMaximized(false);
        }

        // queue+drain, never a lone doLaterLock: two toggles can arm in one
        // dispatch (scripted binds, event backlog) and overwriting the lock
        // cancels the unfired one
        inline CHopQueue<PHLWINDOWREF, 16>& maxToggles() {
            static CHopQueue<PHLWINDOWREF, 16> Q([](std::vector<PHLWINDOWREF>& batch) {
                for (const auto& WR : batch)
                    applyMaxToggle(WR);
                pointerFollowsGeometry();
            });
            return Q;
        }
    }

    // ---- the pipeline handlers (called by the module, before the click
    // ---- policy — the old load-order edge, now in call order) ----

    namespace Max {
        // Buttons whose press we swallowed; their release must be swallowed
        // too so nothing downstream sees half a click.
        inline uint32_t& swallowedButtons() {
            static uint32_t B = 0;
            return B;
        }

        inline void onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
            const uint32_t BIT = trackedPointerButtonBit(e.button) & 3u;

            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (!BIT || info.cancelled)
                    return;

                // the shell owns clicks on its strip; only a Super-grab can
                // move a window, so only that needs swallowing
                if (!superHeld() || nativePointerGrabActive() || nativeLayerOwnsPointer())
                    return;

                const auto W = windowUnderCursor();
                if (!W || !maximizedAny(W))
                    return;

                // immovable: swallow the press before the keybind layer can
                // start a move/resize drag on it
                info.cancelled = true;
                swallowedButtons() |= BIT;
                return;
            }

            if (BIT && (swallowedButtons() & BIT)) {
                swallowedButtons() &= ~BIT;
                info.cancelled = true;
            }
        }

        inline void onInputBlocked() {
            swallowedButtons() = 0;
        }

        inline void requestToggle() {
            const auto FOCUS = Desktop::focusState()->window();
            if (!FOCUS || !FOCUS->mapped() || !FOCUS->m_workspace)
                return;

            maxToggles().push(PHLWINDOWREF{FOCUS});
        }

        inline void init() {
            loadWindowed();

            // the client-unmaximize watch lives as long as the window
            supervisor().listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) {
                if (w)
                    watchClientUnmax(w);
            });
            supervisor().listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) {
                unmaxRequestListeners().erase(wr.get());
                moveRequestListeners().erase(wr.get());
            });

            // A window closed while plugin-maximized: keep its windowed box
            // as the app's remembered size (the window ref itself is about
            // to expire).
            supervisor().listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) {
                const auto IT = maximized().find(wr);
                if (IT == maximized().end())
                    return;
                // get(), never lock(): the emission runs inside ~CWindow,
                // where the ref is already marked destroying — lock() can
                // never succeed there, while the pointed-to members are
                // still intact (destructor body).
                if (const auto* W = wr.get()) {
                    // plugin maximize is a floating mechanism: a window that
                    // left floating (an external re-tile) no longer owns the
                    // box the entry holds — remembering it would save the
                    // workarea as the app's last windowed size
                    if (W->isFloating())
                        rememberWindowed(W->metadata().appID(), monGlobal(IT->second, W->m_monitor.lock()), W->m_monitor.lock());
                }
                maximized().erase(IT);
            });
            // updateState, not window.floating: that one is emitted only by
            // the layout manager's own toggle, while a group merge or a
            // tiled algorithm's forced float leave floating just the same —
            // and this one arrives after the layout has settled
            supervisor().listen(Event::bus()->m_events.window.updateState, [](PHLWINDOW w, const Desktop::View::SWindowStateUpdate& U) {
                if (w && U.floating == false)
                    dropMaximize(w);
            });
            supervisor().listen(Event::bus()->m_events.window.close, [](PHLWINDOW w) {
                if (w)
                    dropMaximize(w);
            });

            // The compositor recomputes the client-facing maximized bit from
            // ITS OWN fullscreen mode on every client-mode change
            // (updateClientMaximizedState) — and this maximize lives outside
            // that machinery, so a video entering/leaving fullscreen
            // stripped the told-maximized state and the browser came back
            // unmaximized. The controller emits this event AFTER its sync, so
            // reasserting here wins, and both changes flush in one configure
            // — the client's belief never flickers.
            //
            // The same event announces a compositor-granted maximize
            // (internal FSMODE_MAXIMIZED): adopt it, deferred — we are inside
            // the controller's emission.
            supervisor().listen(Event::bus()->m_events.window.fullscreen, [](PHLWINDOW w) {
                if (!w)
                    return;
                if (pluginMaximized(w)) {
                    if (auto TOP = xdgToplevel(w))
                        TOP->setMaximized(true);
                    queueReflow();
                    return;
                }
                if (w->isFloating() && Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED)
                    queueAdopt(w);
                queueReflow();
            });

            Tasklist::restoredHook() = []() { queueReflow(); };

            auto& EV = Event::bus()->m_events;
            supervisor().listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { queueReflow(); });
            supervisor().listen(EV.workspace.active, [](PHLWORKSPACE) { queueReflow(); });
            supervisor().listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { queueReflow(); });
            // a special workspace opening on another output carries its
            // windows there with a bare move: refit them to that work area
            supervisor().listen(EV.workspace.specialActive, [](PHLWORKSPACE, PHLMONITOR) { queueReflow(); });
            supervisor().listen(EV.monitor.layoutChanged, []() { queueReflow(); });
            supervisor().listen(EV.monitor.reservedChanged, [](PHLMONITOR) { queueReflow(); });
        }

        inline void teardown() {
            Tasklist::restoredHook() = nullptr;
            maximized().clear();
            swallowedButtons() = 0;
            adopts().reset();
            maxToggles().reset();
            reflowQueued() = false;
            for (auto& [K, L] : unmaxRequestListeners())
                L.reset();
            unmaxRequestListeners().clear();
            clientUnmaxes().reset();
            refusedDrags().reset();
            for (auto& [K, L] : moveRequestListeners())
                L.reset();
            moveRequestListeners().clear();
        }
    } // namespace Max

} // namespace NAwesome::Windows
