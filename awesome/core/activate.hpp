// awesome/core/activate.hpp — explicit activation of an app's own window.
//
// The plugin that SAW the user's gesture (the tray icon click, the
// notification action) performs the activation itself instead of trusting
// the app: resolve the app's bus PID, raise + hard-focus its topmost
// window. This is awesome's guarantee, made unconditional — on X11 the
// app's own activation (_NET_ACTIVE_WINDOW) is unauthenticated (the fork
// maps it to urgency only, the Wine/Proton SetForegroundWindow spam
// would steal focus otherwise), and a Wayland app may simply never spend
// the token the plugin minted for it. Both backends get the focus from
// here; the app's follow-up xdg-activation (if any) lands on an
// already-focused window and is a no-op. Focus here is compositor-side
// (never gated): the user asked for this window.
#pragma once

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>

#include <cstdint>

namespace NAwesome {

// Raise + hard-focus the topmost mapped, visible window of the pid.
// Returns false when the pid has no such window: the app is still
// handling the action (a tray return re-maps a moment later and takes
// the new-window focus, as in awesome).
inline bool activateAppWindow(uint32_t pid) {
    PHLWINDOW best;
    for (const auto& W : Desktop::windowState()->windows()) { // Z-order: the LAST match is topmost
        if (!W->mapped() || W->isHidden() || W->backend().pid() != static_cast<pid_t>(pid))
            continue;
        best = W;
    }
    if (!best)
        return false;
    if (best->isFloating())
        Desktop::windowState()->raise(best);
    Desktop::focusState()->fullWindowFocus(best, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
    return true;
}

} // namespace NAwesome
