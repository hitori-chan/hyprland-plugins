// common/queries.hpp — the read-side helpers every plugin repeats: the
// session-lock gate and the sanctioned cross-plugin channel (xdg protocol
// state — never shared symbols).
#pragma once

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>

#include <algorithm>

namespace NHyprCommon {

    // Input emissions reach plugins BEFORE the compositor's session-lock
    // checks: every input listener gates on this first — and resets its
    // half-tracked state (swallow masks, held counters, armed zones) when
    // it trips.
    inline bool sessionLocked() {
        return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
    }

    // Maximize can be client-only state (hyprmax's maximize never enters
    // compositor fullscreen), so read back what the toplevel was last told.
    inline bool toldMaximized(const PHLWINDOW& w) {
        if (w->m_isX11 || !w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
            return false;
        return std::ranges::contains(w->m_xdgSurface->m_toplevel->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED);
    }

    // A genuinely user-resizable toplevel — its last size is worth
    // restoring (mpv, terminals, browsers). A fixed-size dialog pins
    // min == max in both axes; its size stays the client's, never
    // reimposed (that would blink it — awesome never did). No toplevel
    // (X11, unmapped) = can't tell = treat as fixed.
    inline bool resizable(const PHLWINDOW& w) {
        if (w->m_isX11 || !w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
            return false;
        const auto MIN      = w->m_xdgSurface->m_toplevel->layoutMinSize();
        const auto MAX      = w->m_xdgSurface->m_toplevel->layoutMaxSize();
        const bool PINNED_X = MAX.x > 1 && MIN.x >= MAX.x;
        const bool PINNED_Y = MAX.y > 1 && MIN.y >= MAX.y;
        return !(PINNED_X && PINNED_Y);
    }

} // namespace NHyprCommon
