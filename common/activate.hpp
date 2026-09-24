// common/activate.hpp — explicit activation of an app's own window.
//
// X11 clients cannot authenticate a user gesture: a Wine/Proton app sends
// _NET_ACTIVE_WINDOW (and FlashWindowEx -> DEMANDS_ATTENTION) on every
// internal SetForegroundWindow, and the compositor maps those pings to
// urgency only (XWM) so they can never take focus. The plugin that SAW the
// gesture (the tray icon click, the notification action) performs the
// activation itself: resolve the app's bus PID, raise + hard-focus its
// topmost X11 window.
//
// Only X11 windows need this — a Wayland app self-activates with the
// token-validated xdg-activation protocol (hyprnotify mints the token on
// the ActivationToken signal).
#pragma once

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>

#include <cstdint>

namespace NHyprCommon {

// Raise + hard-focus the topmost mapped X11 window of the pid.
// Returns false when the pid has no such window: the app marked itself
// urgent instead, and the user's next click focuses it.
inline bool activateAppWindow(uint32_t pid) {
    PHLWINDOW best;
    for (const auto& W : Desktop::windowState()->windows()) { // Z-order: the LAST match is topmost
        if (!W->mapped() || W->isHidden() || !W->backend().isX11() || W->backend().pid() != static_cast<pid_t>(pid))
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

} // namespace NHyprCommon
