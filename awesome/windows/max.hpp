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
// AND relogs (persisted under $XDG_STATE_HOME/awesome/): un-maximizing a
// born-maximized window restores it instead of the client's guess (GTK
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

#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace NAwesome::Windows {

    namespace {
        // plugin-maximized windows and their restore geometry.
        inline std::unordered_map<PHLWINDOWREF, CBox>& maximized() {
            static std::unordered_map<PHLWINDOWREF, CBox> M;
            return M;
        }

        // last windowed box per app class, surviving window closes and
        // relogs: the restore target when a window of that app is born
        // maximized again.
        inline BoxStore& lastWindowed() {
            static BoxStore S;
            return S;
        }

        inline std::filesystem::path storePath() {
            return statePath("windows-windowed.tsv");
        }

        inline Saver& saver() {
            static Saver S{[]() { lastWindowed().write(storePath()); }};
            return S;
        }

        inline CBox boundedRestore(PHLWINDOW w, const CBox& box, const CBox& workarea) {
            const auto T = w ? w->windowTarget() : nullptr;
            if (!T)
                return box;
            return Geometry::boundedRestore(box, workarea, T->minSize(), T->maxSize());
        }

        inline void loadWindowed() {
            // one-time migration from the legacy hyprmax store
            migrateBoxStore(storePath(), stateDir().parent_path() / "hyprmax" / "windowed.tsv");
            lastWindowed() = BoxStore::read(storePath());
            // only a real windowed size is a restore target (a legacy
            // position-only row carries none)
            std::erase_if(lastWindowed().rows, [](const auto& E) { return E.second.w <= 5 || E.second.h <= 5; });
        }

        inline void rememberWindowed(const std::string& cls, const CBox& box) {
            if (cls.empty() || box.w <= 5 || box.h <= 5)
                return;
            if (lastWindowed().remember(cls, Box{(int)std::llround(box.x), (int)std::llround(box.y), (int)std::llround(box.w), (int)std::llround(box.h)}))
                saver().dirty();
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
            if (!W || !W->mapped() || !W->windowTarget() || !W->isFloating() || pluginMaximized(W))
                return;
            if (Fullscreen::controller()->getFullscreenModes(W).internal != Fullscreen::FSMODE_MAXIMIZED)
                return;
            const auto MON = W->m_monitor.lock();
            if (!MON)
                return;

            Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
            // the exit just granted the client the size choice — the box is ours
            W->m_sizeFromClientSerial = 0;
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
            if (const auto* B = lastWindowed().find(W->metadata().appID()))
                restore = boundedRestore(W, CBox{(double)B->x, (double)B->y, (double)B->w, (double)B->h}, WA);
            maximized()[PHLWINDOWREF{W}] = restore;

            if (auto TOP = xdgToplevel(W))
                TOP->setMaximized(true);
            // through the layout, never the raw target: the floating
            // algorithm's fullscreen-exit recenter restores ITS tracked
            // lastBox — a raw setPositionGlobal leaves that stale at the
            // pre-maximize box, and the window "un-maximizes" on any later
            // fullscreen roundtrip
            g_layoutManager->setTargetGeom(WA, W->windowTarget());
            W->windowTarget()->warpPositionSize();
            // the exit's 0x0 grant configure already reached the client, but
            // m_pendingReportedSize kept the real size — an unforced send
            // dedups against it and stays silent, leaving the client
            // maximized at 0x0 with nothing to lay out: it never commits a
            // frame (invisible window). Force the workarea configure out.
            W->sendWindowSize(true);
        }

        inline std::vector<PHLWINDOWREF>& adoptQueue() {
            static std::vector<PHLWINDOWREF> Q;
            return Q;
        }
        inline bool& adoptQueued() {
            static bool Q = false;
            return Q;
        }
        inline CHop& pendingAdopt() {
            static CHop H;
            return H;
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
        inline void reflowMaximized() {
            for (const auto& ENTRY : maximized()) {
                const auto W = ENTRY.first.lock();
                if (!W || !W->mapped() || !W->isFloating() || !W->windowTarget())
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
                g_layoutManager->setTargetGeom(WA, W->windowTarget());
                W->windowTarget()->warpPositionSize();
            }
        }

        inline void queueReflow() {
            if (reflowQueued())
                return;
            reflowQueued() = true;
            pendingReflow().arm([]() {
                reflowQueued() = false;
                reflowMaximized();
            });
        }

        // queue+drain, never a lone doLaterLock: two born-maximized windows
        // can map in one dispatch, and overwriting the lock cancels the
        // unfired one
        inline void queueAdopt(PHLWINDOW w) {
            adoptQueue().emplace_back(w);
            if (adoptQueued())
                return;
            adoptQueued() = true;
            pendingAdopt().arm([]() {
                adoptQueued() = false;
                const auto Q = std::move(adoptQueue());
                adoptQueue().clear();
                for (const auto& WR : Q)
                    adoptCompositorMax(WR.lock());
            });
        }

        inline bool maximizedAny(PHLWINDOW w) {
            return pluginMaximized(w) || Fullscreen::controller()->isFullscreen(w);
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
                const auto  B  = lastWindowed().find(W->metadata().appID());
                if (B && W->m_sizeFromClientSerial && MON && W->isFloating()) {
                    W->m_sizeFromClientSerial = 0;
                    g_layoutManager->setTargetGeom(boundedRestore(W, CBox{(double)B->x, (double)B->y, (double)B->w, (double)B->h}, MON->logicalBoxMinusReserved()), W->windowTarget());
                    W->windowTarget()->warpPositionSize();
                    // same disarmed-grant flush as adoptCompositorMax:
                    // unforced sends dedup against m_pendingReportedSize and
                    // go silent when the remembered box matches it
                    W->sendWindowSize(true);
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

            // X11 has no maximize hint on this path; geometry alone.
            const auto setClientMaximized = [&W](bool m) {
                if (auto TOP = xdgToplevel(W))
                    TOP->setMaximized(m);
            };

            const auto WA = MON->logicalBoxMinusReserved();

            if (const auto IT = maximized().find(WR); IT != maximized().end()) {
                const CBox STORED = IT->second;
                maximized().erase(IT);
                setClientMaximized(false);
                if (STORED.w > 5 && STORED.h > 5) {
                    const CBox R = boundedRestore(W, STORED, WA);
                    rememberWindowed(W->metadata().appID(), R);
                    g_layoutManager->setTargetGeom(R, W->windowTarget());
                    W->windowTarget()->warpPositionSize();
                } else {
                    // adopted with no remembered box: the client picks its
                    // size (the 0x0 grant; the commit adoption recenters it)
                    W->requestClientSize();
                }
            } else {
                const auto BOX = W->windowTarget()->position();
                maximized().emplace(WR, BOX);
                rememberWindowed(W->metadata().appID(), BOX);
                setClientMaximized(true);
                g_layoutManager->setTargetGeom(WA, W->windowTarget());
                W->windowTarget()->warpPositionSize();
                Desktop::windowState()->raise(W);
            }
        }

        // queue+drain, never a lone doLaterLock: two toggles can arm in one
        // dispatch (scripted binds, event backlog) and overwriting the lock
        // cancels the unfired one
        inline std::vector<PHLWINDOWREF>& maxToggles() {
            static std::vector<PHLWINDOWREF> Q;
            return Q;
        }
        inline bool& maxToggleQueued() {
            static bool Q = false;
            return Q;
        }
        inline CHop& pendingMax() {
            static CHop H;
            return H;
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
                if (nativePointerGrabActive() || nativeLayerOwnsPointer() || !superHeld())
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

            if (maxToggles().size() < 16)
                maxToggles().emplace_back(PHLWINDOWREF{FOCUS});
            if (maxToggleQueued())
                return;
            maxToggleQueued() = true;
            pendingMax().arm([]() {
                maxToggleQueued() = false;
                const auto Q = std::move(maxToggles());
                maxToggles().clear();
                for (const auto& WR : Q)
                    applyMaxToggle(WR);
            });
        }

        inline void init() {
            loadWindowed();

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
                        rememberWindowed(W->metadata().appID(), IT->second);
                }
                maximized().erase(IT);
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

            auto& EV = Event::bus()->m_events;
            supervisor().listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { queueReflow(); });
            supervisor().listen(EV.workspace.active, [](PHLWORKSPACE) { queueReflow(); });
            supervisor().listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { queueReflow(); });
            supervisor().listen(EV.monitor.layoutChanged, []() { queueReflow(); });
            supervisor().listen(EV.monitor.reservedChanged, [](PHLMONITOR) { queueReflow(); });
        }

        inline void teardown() {
            saver().flush(); // the coalesced write must not die with the session
            maximized().clear();
            lastWindowed().rows.clear();
            swallowedButtons() = 0;
            adoptQueued() = false;
            reflowQueued() = false;
            adoptQueue().clear();
        }
    } // namespace Max

} // namespace NAwesome::Windows
