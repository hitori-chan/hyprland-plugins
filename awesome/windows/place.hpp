// awesome/windows/place.hpp — spawn placement for floating windows:
//
//   1. an app reopens where its last window closed (per class, persisted
//      across relogs): the remembered spot lands when it's free — a
//      sibling sitting on it sends the newcomer to step 2 instead. A close
//      in maximized state carries the app's last windowed box (the
//      maximize module's restore memory) into the spot, so an app that
//      always closes maximized keeps its spawn memory
//   2. otherwise the spot that overlaps the other windows the least —
//      KWin's default. A lone window keeps the compositor's centered spot
//      (nothing to overlap), a busy screen fills the gaps, and a full one
//      lands where it hides the least. No cascade, no center pile.
//
// Placement only ever MOVES a window; its size is the client's own. A
// Wayland toplevel can't position itself, so the compositor remembers
// where it was (memory-first, as macOS window restoration and Windows
// SetWindowPlacement do for their apps); but it does choose its size —
// the initial configure is 0x0, "you decide" — and real apps restore
// their own (the GTK file chooser, Firefox, Thunar, Telegram), while a
// dialog's natural size is its content's. Imposing a remembered size
// overrode both: every dialog of a class was born at the app's main-window
// size (the portal file chooser at 1203x953). The unmaximize restore box
// (max.hpp) is the one size the compositor supplies, because a window born
// maximized has no windowed size of its own.
//
// Windows that chose their spot (X11, dialogs anchored to a parent) keep
// it while it's free; a fixed-size toplevel (min == max — a dialog, a
// splash) keeps the compositor's centered spot outright and never reads or
// writes the class row, so a splash's box can neither steer it to a corner
// nor clobber the app's memory; X11 override-redirect surfaces are left
// alone; the result is clamped fully on-screen, border included
// (no_offscreen), unless the window is too big to fit. Maximized windows
// AND floats sized to the whole workarea consume no free space; the
// placement scan then puts a new window where it overlaps them the least.
#pragma once

#include "state.hpp"

#include "core/queries.hpp"
#include "core/state.hpp"

#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace NAwesome::Windows::Place {

    namespace {
        // each app's last window box (position + size), surviving relogs; the
        // legacy position-only rows load with a zero size, which stays until
        // the app closes once and a full box is recorded. The store is the
        // core's unified state (core/state.hpp).
        inline BoxStore& lastSpot() {
            return StateStore::inst().data().spot;
        }

        // metadata().appID() is captured on first map; a window that never
        // got there still has its class on the client state
        inline std::string classKey(PHLWINDOW w) {
            if (!w)
                return {};
            if (!w->metadata().appID().empty())
                return w->metadata().appID();
            if (const auto TOP = xdgToplevel(w))
                return TOP->m_state.appid;
            return {};
        }

        inline void rememberSpot(const std::string& cls, const CBox& box) {
            if (lastSpot().remember(cls, Box{(int)std::llround(box.x), (int)std::llround(box.y), (int)std::llround(box.w), (int)std::llround(box.h)}))
                StateStore::inst().dirty();
        }

        // a float sized to (or past) the whole workarea is maximized in all
        // but state — it consumes no free space, and its close-spot is not a
        // spot (a workarea-sized browser once blanked ALL free area, sending
        // every spawn to the same center)
        inline bool coversWorkarea(const CBox& b, const CBox& wa) {
            return b.x <= wa.x && b.y <= wa.y && b.x + b.w >= wa.x + wa.w && b.y + b.h >= wa.y + wa.h;
        }

        // The open emission precedes Hyprland's initial fullscreen/maximize
        // application. Do not place or resize a float while any grant is
        // still pending, and do not mistake a compositor mode for a normal
        // client-chosen geometry. This mirrors the target's map-time state
        // sources instead of trying to infer them from a placeholder box.
        // Read the REQUESTS, not the toplevel's applied states: a state
        // check would read a grant the compositor has already told the
        // client about, not one that is about to be applied — the request
        // flags (plus the rule grants and the controller's modes) are the
        // map-time sources of truth for "a grant is still in flight".
        inline bool hasFullscreenOrMaximizeGrant(PHLWINDOW w) {
            if (!w)
                return false;
            if (w->fullscreenPolicy().pendingClientRequest().mode.has_value())
                return true;

            if (const auto TOP = xdgToplevel(w);
                TOP && (TOP->m_state.requestsFullscreen.value_or(false) || TOP->m_state.requestsMaximize.value_or(false)))
                return true;

            // X11: a fullscreen client state is a trait; its maximize and
            // fullscreen requests ride the pending client request above
            if (w->backend().isX11() && w->backend().traits().fullscreen)
                return true;

            if (w->m_ruleApplicator) {
                const auto& STATIC = w->m_ruleApplicator->static_;
                if (STATIC.fullscreen.value_or(false) || STATIC.maximize.value_or(false) || STATIC.fullscreenStateInternal.value_or(0) != 0 ||
                    STATIC.fullscreenStateClient.value_or(0) != 0)
                    return true;
            }

            const auto MODES = Fullscreen::controller()->getFullscreenModes(w);
            return MODES.internal != Fullscreen::FSMODE_NONE || MODES.client != Fullscreen::FSMODE_NONE;
        }

        inline void placeWindow(PHLWINDOW w) {
            // X11 override-redirect surfaces (menus, tooltips) place
            // themselves
            if (!w || !w->mapped() || !w->isFloating() || w->backend().traits().overrideRedirect || !w->windowTarget() ||
                Fullscreen::controller()->isFullscreen(w))
                return;
            if (hasFullscreenOrMaximizeGrant(w))
                return;
            const auto WS  = w->m_workspace;
            const auto MON = w->m_monitor.lock();
            if (!WS || !MON)
                return;

            const auto WA  = MON->logicalBoxMinusReserved();
            const auto CUR = w->windowTarget()->position();

            // a client-maximized or workarea-filling window is not ours to
            // place or resize — symmetric with onWindowClose. Without this
            // we reimpose a born-maximized app's old windowed box and
            // silently un-maximize it (isFullscreen alone misses it: the
            // maximize never enters compositor fullscreen).
            if (toldMaximized(w) || coversWorkarea(CUR, WA))
                return;

            // the visible floating windows to stay clear of; maximized and
            // fullscreen ones cover no free space, as in awesome. A dialog's
            // own parent is not an obstacle: the compositor centered it there
            // on purpose, and counting the parent sent every parented dialog
            // off it to the least-overlap spot.
            const auto        PARENT = w->backend().parent();
            std::vector<CBox> blockers;
            for (const auto& O : Desktop::windowState()->windows()) {
                if (O == w || O == PARENT || !O->mapped() || O->isHidden() || !O->isFloating() || !O->windowTarget())
                    continue;
                if (O->m_workspace != WS && !(isPinned(O) && O->m_monitor.lock() == MON))
                    continue;
                if (Fullscreen::controller()->isFullscreen(O) || toldMaximized(O))
                    continue;
                const auto OB = O->windowTarget()->position();
                if (coversWorkarea(OB, WA))
                    continue;
                blockers.push_back(OB);
            }

            const auto fits = [&](const CBox& b) {
                if (b.x < WA.x || b.y < WA.y || b.x + b.w > WA.x + WA.w || b.y + b.h > WA.y + WA.h)
                    return false;
                for (const auto& B : blockers)
                    if (b.x < B.x + B.w && b.x + b.w > B.x && b.y < B.y + B.h && b.y + b.h > B.y)
                        return false;
                return true;
            };

            // the client's own size, always; the spot only lands when free
            // (fits() below), so a sibling sitting on it sends the newcomer
            // to least-overlap, never onto an exact stack
            const Vector2D size = CUR.size();
            // A fixed-size native toplevel (min == max) is a dialog or a
            // splash, not an app window: it keeps the compositor's native
            // placement and stays out of the class memory in both
            // directions. Reading would steer a blocked remembered spot into
            // the least-overlap corner — and a class whose main window is
            // grant-exempt (a maximized Electron main) lets the splash own
            // the row, so the corner lands, closes and is remembered again;
            // writing would clobber the row with the transient's box. X11
            // and parent-anchored windows below keep their own
            // keep-while-free contract.
            if (!resizable(w) && !w->backend().isX11() && !w->backend().parent())
                return;
            // the remembered spot: its position (the row keeps the close-box)
            std::optional<Vector2D> stored;
            if (!w->backend().isX11() && !w->backend().parent())
                if (const auto B = lastSpot().find(classKey(w)); B)
                    stored = Vector2D{(double)B->x, (double)B->y};

            // no_offscreen: nudge the box fully into the workarea AND leave
            // a border's width of margin — the border is drawn outside the
            // box, so a box flush to the workarea edge clips it. Used for the
            // remembered spot and the final placement alike, so a window
            // dragged against an edge before close reopens against it,
            // border shown, never discarded to center. A window too big to
            // fit even without the margin drops it on that axis rather than
            // going off-screen — and a maximized/workarea-filling window,
            // wider than the margin allows, is left exactly where it is.
            const double BORDER    = std::max(0, w->presentation().borderSize());
            const auto   clampToWA = [&](const Vector2D& p) {
                const double mx  = size.x + 2 * BORDER <= WA.w ? BORDER : 0;
                const double my  = size.y + 2 * BORDER <= WA.h ? BORDER : 0;
                const double loX = WA.x + mx, hiX = WA.x + WA.w - mx - size.x;
                const double loY = WA.y + my, hiY = WA.y + WA.h - my - size.y;
                return Vector2D{std::clamp(p.x, loX, std::max(loX, hiX)), std::clamp(p.y, loY, std::max(loY, hiY))};
            };

            std::optional<Vector2D> pos;

            if (w->backend().isX11() || w->backend().parent()) {
                // the window chose this spot (X11 geometry, parent-anchored
                // dialog): keep it while it's free
                if (fits(CBox{CUR.pos(), size}))
                    return;
            } else {
                // 1: where this app's last window closed, clamped on-screen
                // so a spot that ran past an edge is honored (against the
                // edge) rather than lost
                if (stored) {
                    const auto P = clampToWA(*stored);
                    if (fits(CBox{P, size}))
                        pos = P;
                }
            }

            // Memory missed (or its spot is taken): least-overlap placement,
            // KWin's default. A least-overlap top-left always sits at a grid
            // point of the windows' own edges (and the workarea corner), so
            // score the window there and keep the clearest — starting from,
            // and so preferring, its current centered spot. A lone window
            // stays centered, a busy screen fills the gaps top-left first, a
            // full one hides where it can (windows wider than the free space
            // have no gap and settle into the corners). One pass, no
            // cascade, no center pile.
            if (!pos) {
                // cutoff: only a strictly better score wins, so the running
                // total can disqualify a point the moment it reaches it — a
                // busy screen scores (2n+1)^2 grid points, and the losers
                // stop early
                const auto overlapAt = [&](const Vector2D& p, double cutoff) {
                    double sum = 0.0;
                    for (const auto& B : blockers) {
                        const double ix = std::min(p.x + size.x, B.x + B.w) - std::max(p.x, B.x);
                        const double iy = std::min(p.y + size.y, B.y + B.h) - std::max(p.y, B.y);
                        if (ix > 0 && iy > 0) {
                            sum += ix * iy;
                            if (sum >= cutoff)
                                return sum;
                        }
                    }
                    return sum;
                };

                // dedup so an aligned grid of windows stays a few
                // coordinates
                std::vector<double> xs{WA.x}, ys{WA.y};
                for (const auto& B : blockers) {
                    xs.insert(xs.end(), {B.x, B.x + B.w});
                    ys.insert(ys.end(), {B.y, B.y + B.h});
                }
                std::sort(xs.begin(), xs.end());
                std::sort(ys.begin(), ys.end());
                xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
                ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

                Vector2D best   = clampToWA(CUR.pos());
                double   bestOv = overlapAt(best, std::numeric_limits<double>::infinity());
                for (const double X : xs) {
                    if (bestOv <= 1.0) // a zero-overlap gap — nothing beats it
                        break;
                    for (const double Y : ys) {
                        const Vector2D P  = clampToWA(Vector2D{X, Y});
                        const double   OV = overlapAt(P, bestOv);
                        if (OV < bestOv - 1.0) {
                            bestOv = OV;
                            best   = P;
                        }
                    }
                }
                pos = best;
            }

            const Vector2D chosen = pos.value_or(CUR.pos());

            // no_offscreen: clamp into the workarea
            const auto   FINAL = clampToWA(chosen);
            const double nx = FINAL.x, ny = FINAL.y;

            if (nx == CUR.x && ny == CUR.y)
                return;
            // through the layout so the floating algorithm's lastBox
            // tracking follows the placement (a raw target move leaves it
            // stale and a fullscreen roundtrip would restore the
            // pre-placement spot); a move, never a configure
            setGeom(w, CBox{nx, ny, size.x, size.y});
            w->windowTarget()->warpPositionSize();
        }
    }

    // deferred out of the map emission; runs before the first frame
    // renders. Several windows can map in one dispatch — queued, one drain.
    // Past 256 pending maps the overload falls back to Hyprland's native
    // placement.
    inline CHopQueue<PHLWINDOWREF, 256>& places() {
        static CHopQueue<PHLWINDOWREF, 256> Q([](std::vector<PHLWINDOWREF>& batch) {
            for (const auto& REF : batch)
                placeWindow(REF.lock());
        });
        return Q;
    }

    inline void onWindowOpen(PHLWINDOW w) {
        if (w)
            places().push(PHLWINDOWREF{w});
    }

    inline void onWindowClose(PHLWINDOW w) {
        // a maximized/fullscreen close-box is the workarea, not a spot; X11
        // windows and dialogs place themselves and never consult the
        // memory; a fixed-size native window (a dialog/splash) never owns
        // the row, or its transient box would clobber the app's remembered
        // close-box
        if (!w || !w->mapped() || !w->isFloating() || !w->windowTarget() || w->backend().isX11() || w->backend().parent() ||
            !resizable(w))
            return;
        const auto CLS = classKey(w);
        if (toldMaximized(w) || Fullscreen::controller()->isFullscreen(w))
        {
            // a browser that always closes maximized would otherwise never
            // leave a spot row: its spawn memory would be lost and it would
            // reopen centered at the client's own size. The app's last
            // WINDOWED box (the maximize module's restore memory) is where
            // it actually was — carry it into the spot.
            if (const auto WB = StateStore::inst().data().windowed.find(CLS); WB && WB->w > 5 && WB->h > 5)
                rememberSpot(CLS, CBox{(double)WB->x, (double)WB->y, (double)WB->w, (double)WB->h});
            return;
        }
        if (const auto MON = w->m_monitor.lock(); MON && coversWorkarea(w->windowTarget()->position(), MON->logicalBoxMinusReserved()))
        {
            // a workarea-filling float is the plugin-maximize shape: same
            // fallback, or its close-box would read as a spot.
            if (const auto WB = StateStore::inst().data().windowed.find(CLS); WB && WB->w > 5 && WB->h > 5)
                rememberSpot(CLS, CBox{(double)WB->x, (double)WB->y, (double)WB->w, (double)WB->h});
            return;
        }
        rememberSpot(CLS, w->windowTarget()->position());
    }

    inline void init() {
        // the unified state (and the one-time legacy migration) is loaded
        // by the supervisor before module inits
        supervisor().listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) { onWindowOpen(w); });
        supervisor().listen(Event::bus()->m_events.window.close, [](PHLWINDOW w) { onWindowClose(w); });
    }

    inline void teardown() {
        places().reset();
        lastSpot().rows.clear();
    }

} // namespace NAwesome::Windows::Place
