// awesome/core/queries.hpp — the read-side of the compositor: the session
// lock gate, the native input stack, and the state lookups every module
// shares. The helpers encode input-ordering regressions (crash classes 3
// and 7) — treat them as pinned contracts, not candidates for cleanup.
#pragma once

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <linux/input-event-codes.h>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputMethodRelay.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/InputCapture.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>

#include <algorithm>
#include <limits>

namespace NAwesome {

    // Input emissions reach plugins BEFORE the compositor's session-lock
    // checks: every input handler gates on this first — and resets its
    // half-tracked state (swallow masks, held counters, armed zones) when
    // it trips.
    inline bool sessionLocked() {
        return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
    }

    // The compositor-side teardown: CCompositor::cleanup() destroys the
    // workspace/window/monitor state while the plugin is still loaded —
    // the unload deliberately happens LATER (the renderer holds smart
    // refs into the .so, and plugin-held textures must die with GL alive),
    // so ~CWindow & co. still emit bus events into our listeners while the
    // monitors' GL resources are dying. A listener that warms the bar into
    // that half-dead state SEGVs on a released CSharedPointer owner (the
    // 2026-10-01 "input storm SEGV": every nested teardown, 100%). Every
    // event-driven path must no-op from the moment this is true; the
    // supervisor gates its state listeners on it.
    inline bool compositorShuttingDown() {
        return g_pCompositor && g_pCompositor->m_isShuttingDown;
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
        return PROTO::inputCapture && PROTO::inputCapture->isCaptured();
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

    // A compositor-drawn overlay may cancel mouse motion before Hyprland can
    // refresh m_lastFocusOnLS. Resolve the native stack at the event point so
    // it cannot swallow input belonging to an IME popup, a focus-priority
    // window, a stay-focused window, or a layer surface. Mirrors the order of
    // CInputManager::mouseMoveUnified() — re-check it on every fork bump.
    // Read-only, allocation-free: it runs per pointer motion.
    inline bool nativeLayerOwnsPointer() {
        if (!g_pInputManager)
            return false;
        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON)
            return false;

        if (g_pInputManager->m_relay.popupFromCoords(POS))
            return true;

        const auto HIT = Desktop::viewState()->hitTest();
        Vector2D surfaceCoords;
        if (const auto PRIORITY = HIT.windowAt(POS, Desktop::View::FOCUS_PRIORITY); PRIORITY && HIT.windowSurfaceAt(POS, PRIORITY, surfaceCoords))
            return true;

        if (!g_pInputManager->m_forcedFocus.expired())
            return true;
        for (const auto& W : Desktop::windowState()->windows()) // the stay-focused query, without its query object
            if (W->mapped() && W->acceptsInput() && W->m_workspace && W->m_workspace->visible() && W->m_ruleApplicator->stayFocused().valueOrDefault())
                return true;

        const auto WS = MON->m_activeSpecialWorkspace ? MON->m_activeSpecialWorkspace : MON->m_activeWorkspace;
        const auto& FS = Fullscreen::controller();
        const bool EXCLUSIVE_FS = (FS->hasFullscreen(WS) && FS->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN) ||
            (FS->hasFullscreen(MON) && FS->getFullscreenModes(MON).internal == Fullscreen::FSMODE_FULLSCREEN);
        // the mouseMoveUnified() fullscreen filter: a top layer without
        // aboveFullscreen sits below an exclusive fullscreen client
        const auto OWNS = [EXCLUSIVE_FS](const PHLLS& ls) {
            return ls &&
                (!EXCLUSIVE_FS || ls->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
                 (ls->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP && (ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN)));
        };

        PHLLS layer;
        if (HIT.layerPopupSurfaceAt(POS, MON, &surfaceCoords, &layer))
            return OWNS(layer);
        if (HIT.layerSurfaceAt(POS, &MON->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &layer))
            return OWNS(layer);
        return HIT.layerSurfaceAt(POS, &MON->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &layer) && OWNS(layer);
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

    // The client-facing xdg toplevel, or nullptr for X11 windows, unmapped
    // views, or a destroyed role (a closing GTK/Firefox client destroys its
    // toplevel before the window unmaps). Reached through the surface's
    // public role object.
    inline SP<CXDGToplevelResource> xdgToplevel(const PHLWINDOW& w) {
        if (!w || w->backend().isX11() || !w->wlSurface())
            return nullptr;
        const auto SURF = w->wlSurface()->resource();
        if (!SURF || !SURF->m_role || SURF->m_role->role() != SURFACE_ROLE_XDG_SHELL)
            return nullptr;
        const auto XDG = static_cast<CXDGSurfaceRole*>(SURF->m_role.get())->m_xdgSurface.lock();
        return XDG ? XDG->m_toplevel.lock() : nullptr;
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

    // The workspace a monitor shows on top: an open special workspace
    // covers the regular one, and its windows are the ones a cycle or a
    // magnet means.
    inline PHLWORKSPACE shownWorkspace(const PHLMONITOR& mon) {
        if (!mon)
            return nullptr;
        return mon->m_activeSpecialWorkspace ? mon->m_activeSpecialWorkspace : mon->m_activeWorkspace;
    }

    // A window in the maximized presentation. Read the presentation, not
    // the xdg MAXIMIZED state: the compositor's internal FSMODE_MAXIMIZED
    // covers born/client maximize, and a float whose box covers the whole
    // workarea is the windows module's plugin-maximize box (a float sized
    // to the workarea is maximized in all but state) — the geometry is the
    // one fact both mechanisms share. (The fork no longer lies maximized
    // at map, so the client state is a true read-back again; the
    // presentation read stays because it also covers the plugin box.)
    inline bool toldMaximized(const PHLWINDOW& w) {
        if (!w || !w->windowTarget())
            return false;
        if (Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED)
            return true;
        if (!w->isFloating())
            return false;
        const auto MON = w->m_monitor.lock();
        if (!MON)
            return false;
        const auto CUR = w->windowTarget()->position();
        const auto WA  = MON->logicalBoxMinusReserved();
        return CUR.x <= WA.x && CUR.y <= WA.y && CUR.x + CUR.w >= WA.x + WA.w && CUR.y + CUR.h >= WA.y + WA.h;
    }

    // A genuinely user-resizable toplevel — its last size is worth
    // restoring (mpv, terminals, browsers). A fixed-size dialog pins
    // min == max in both axes; its size stays the client's, never
    // reimposed (that would blink it — awesome never did). X11 or no hints
    // ever committed = can't tell = treat as fixed.
    //
    // Read the backend's hints, not the xdg_toplevel: they are cached past
    // the toplevel's death, and a closing client (GTK, Firefox) destroys
    // its toplevel BEFORE the window unmaps — the close-time spot read
    // then saw "no toplevel", took the window for fixed-size and never
    // remembered where any of those apps closed.
    inline bool resizable(const PHLWINDOW& w) {
        if (!w || w->backend().isX11())
            return false;
        const auto HINTS = w->backend().geometryHints(Desktop::View::eBackendState::BACKEND_STATE_CURRENT);
        if (!HINTS.minSize || !HINTS.maxSize)
            return false;
        const auto MIN      = *HINTS.minSize;
        const auto MAX      = *HINTS.maxSize;
        const auto UNBOUND  = std::numeric_limits<double>::max(); // the backend's "no max" sentinel
        const bool PINNED_X = MAX.x < UNBOUND && MIN.x >= MAX.x;
        const bool PINNED_Y = MAX.y < UNBOUND && MIN.y >= MAX.y;
        return !(PINNED_X && PINNED_Y);
    }

    // Only buttons with an action in the shell may be swallowed. Other
    // buttons remain native application input and must never share a mask.
    inline uint32_t trackedPointerButtonBit(uint32_t button) noexcept {
        switch (button) {
            case BTN_LEFT:
                return 1u;
            case BTN_RIGHT:
                return 2u;
            case BTN_MIDDLE:
                return 4u;
            default:
                return 0u;
        }
    }

} // namespace NAwesome
