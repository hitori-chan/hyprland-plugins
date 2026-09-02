// common/queries.hpp — the read-side helpers every plugin repeats: the
// session-lock gate and the sanctioned cross-plugin channel (xdg protocol
// state — never shared symbols).
#pragma once

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WaylandBackend.hpp>
#include <hyprland/src/desktop/view/window/X11Backend.hpp>
#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>

namespace NHyprCommon {

    // Input emissions reach plugins BEFORE the compositor's session-lock
    // checks: every input listener gates on this first — and resets its
    // half-tracked state (swallow masks, held counters, armed zones) when
    // it trips.
    inline bool sessionLocked() {
        return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
    }

    // A compositor-drawn overlay may cancel mouse motion before Hyprland can
    // refresh m_lastFocusOnLS. Resolve the native stack at the event point so
    // it cannot swallow input belonging to a priority window, layer surface,
    // or IME popup.
    inline bool nativeLayerOwnsPointer() {
        return g_pInputManager && g_pInputManager->pointerHitIsNativeSurface();
    }

    // Input listeners run before CInputManager records the current press, so
    // the held-button half covers an existing client implicit grab. A native
    // seat grab covers xdg-popups and focus-grab surfaces even with no button
    // held; both must beat compositor-drawn plugin surfaces.
    inline bool nativePointerGrabActive() {
        return (g_pInputManager && g_pInputManager->hasHeldButtons()) ||
            (g_pSeatManager && g_pSeatManager->m_seatGrab && g_pSeatManager->m_seatGrab->m_pointer);
    }

    // Input-capture-v1 is fed after plugin emissions. A capture client owns
    // the physical event stream, so compositor-drawn plugin surfaces must not
    // cancel a button, axis, key, or warp event before it reaches that client.
    inline bool nativeInputCaptureActive() {
        return g_pInputManager && g_pInputManager->inputCaptureActive();
    }

    // A REAL fullscreen window owns this monitor's active workspace (a
    // maximized one respects the reserved strip and does not count). The bar
    // hides for it; notification quieting is an explicit opt-in policy.
    inline bool fullscreenOn(PHLMONITOR mon) {
        if (!mon || !Fullscreen::controller())
            return false;
        const auto WS = mon->m_activeWorkspace;
        return WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN;
    }

    // The monitor a point belongs to, nearest one if it lands off every
    // output. monitorState()->query().vec().run() allocates and RTTI-casts
    // per call and the input listeners call this per pointer motion, so
    // mirror closestTo directly instead.
    inline PHLMONITOR monitorAt(const Vector2D& pos) {
        PHLMONITOR best;
        float      bestDist = 0.F;
        for (const auto& M : State::monitorState()->monitors()) {
            const auto BOX = M->logicalBox();
            if (BOX.containsPoint(pos))
                return M;
            const float DIST = vecToRectDistanceSquared(pos, BOX.pos(), BOX.pos() + BOX.size());
            if (!best || DIST < bestDist) {
                best     = M;
                bestDist = DIST;
            }
        }
        return best;
    }

    // A containing output is different from monitorAt(): edge-triggered UI
    // must not arm on the nearest output while the pointer is in a gap.
    inline PHLMONITOR monitorContaining(const Vector2D& pos) {
        for (const auto& M : State::monitorState()->monitors())
            if (M->logicalBox().containsPoint(pos))
                return M;
        return nullptr;
    }

    // The window under the pointer, hit-tested FRESH — never the seat's
    // pointer focus, which a map or unmap under a still cursor leaves stale.
    // Reserved and input extents count: a click on a window's shadow or CSD
    // border is a click on that window.
    inline PHLWINDOW windowUnderCursor() {
        if (!g_pInputManager)
            return nullptr;
        return Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                        Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);
    }

    // Super/Meta down on the seat's keyboard — the modifier every grab chord
    // in the shell is built on.
    inline bool superHeld() {
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        return KB && (KB->getModifiers() & Input::HL_MODIFIER_META) != Input::HL_MODIFIER_NONE;
    }

    // The client-facing xdg toplevel role resource, or nullptr for X11
    // windows, unmapped views, or a destroyed resource. The fork exposes
    // CWaylandBackend::m_resource publicly for exactly this (see
    // docs/audit-tracking.md); the old fork's m_xdgSurface->m_toplevel chain
    // is gone in the backend split.
    inline SP<CXDGToplevelResource> xdgToplevel(const PHLWINDOW& w) {
        if (!w || w->backend().isX11())
            return nullptr;
        const auto* wl = dynamic_cast<const Desktop::View::CWaylandBackend*>(&w->backend());
        if (!wl)
            return nullptr;
        const auto res = wl->m_resource.lock();
        return res ? res->m_toplevel.lock() : nullptr;
    }

    // The X11 surface of an XWayland window, or nullptr. Same fork exposure
    // as xdgToplevel() (CX11Backend::m_xwaylandSurface).
    inline SP<CXWaylandSurface> x11Surface(const PHLWINDOW& w) {
        if (!w || !w->backend().isX11())
            return nullptr;
        const auto* x11 = dynamic_cast<const Desktop::View::CX11Backend*>(&w->backend());
        if (!x11)
            return nullptr;
        return x11->m_xwaylandSurface.lock();
    }

    // "Pinned" = the `pin` window rule (all-workspace/ontop). Post backend
    // split this is a window-state bit (the old per-window bool is gone); the
    // fork's own isAllowedOverFullscreen() consumes this same bit. This is
    // NOT the client-told maximize (FSMODE_MAXIMIZED) — a separate concept
    // that must not skip these loops (it used to get its over-fullscreen
    // grant cleared, and must keep doing so).
    inline bool isPinned(const PHLWINDOW& w) {
        return w && static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
    }

    // Maximize can be client-only state (hyprmax's maximize never enters
    // compositor fullscreen), so read back what the toplevel was last told.
    inline bool toldMaximized(const PHLWINDOW& w) {
        const auto TOP = xdgToplevel(w);
        if (!TOP)
            return false;
        return std::ranges::contains(TOP->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED);
    }

    // A genuinely user-resizable toplevel — its last size is worth
    // restoring (mpv, terminals, browsers). A fixed-size dialog pins
    // min == max in both axes; its size stays the client's, never
    // reimposed (that would blink it — awesome never did). No toplevel
    // (X11, unmapped) = can't tell = treat as fixed.
    inline bool resizable(const PHLWINDOW& w) {
        const auto TOP = xdgToplevel(w);
        if (!TOP)
            return false;
        const auto MIN      = TOP->layoutMinSize();
        const auto MAX      = TOP->layoutMaxSize();
        const bool PINNED_X = MAX.x > 1 && MIN.x >= MAX.x;
        const bool PINNED_Y = MAX.y > 1 && MIN.y >= MAX.y;
        return !(PINNED_X && PINNED_Y);
    }

} // namespace NHyprCommon
