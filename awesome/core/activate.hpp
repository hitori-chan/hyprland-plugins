// awesome/core/activate.hpp — explicit activation of an app's own window.
//
// The plugin that SAW the user's gesture (the tray icon click, the
// notification action) performs the activation itself instead of trusting
// the app: resolve the app's bus PID, raise + hard-focus its topmost
// window. This is awesome's guarantee, made unconditional — on X11 the
// app's own activation (_NET_ACTIVE_WINDOW) is unauthenticated (with
// focus_on_activate off, the default, it only marks urgent — the
// Wine/Proton SetForegroundWindow spam would steal focus otherwise), and a
// Wayland app may simply never spend the token the plugin minted for it. Both backends get the focus from
// here; the app's follow-up xdg-activation (if any) lands on an
// already-focused window and is a no-op. Focus here is compositor-side
// (never gated): the user asked for this window.
#pragma once

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>

#include <cstdint>
#include <functional>

namespace NAwesome {

// The windows module's minimize lives in the windows module: it registers
// how to restore a minimized window here (cleared in its teardown), so an
// activation of a minimized app restores it instead of finding nothing.
inline std::function<bool(const PHLWINDOW&)>& restoreMinimizedHook() {
    static std::function<bool(const PHLWINDOW&)> H;
    return H;
}

// Raise + hard-focus the topmost mapped, visible window of the pid; failing
// that, restore its topmost minimized window (hidden is not minimized: a
// swallowed window is hidden too, and the hook refuses it). Returns false
// when the pid has no such window: the app is still handling the action (a
// tray return re-maps a moment later and takes the new-window focus, as in
// awesome).
inline bool activateAppWindow(uint32_t pid) {
    const auto& WINDOWS = Desktop::windowState()->windows(); // Z-order: the LAST match is topmost
    PHLWINDOW   best;
    for (const auto& W : WINDOWS)
        if (W->mapped() && !W->isHidden() && W->backend().pid() == static_cast<pid_t>(pid))
            best = W;
    if (!best) {
        if (!restoreMinimizedHook())
            return false;
        for (size_t i = WINDOWS.size(); i-- > 0;) {
            // a copy, never a reference into the list: the restore raises,
            // which rotates the compositor's window vector under slot i
            const PHLWINDOW W = WINDOWS[i];
            if (W->mapped() && W->isHidden() && W->backend().pid() == static_cast<pid_t>(pid) && restoreMinimizedHook()(W))
                return true;
        }
        return false;
    }
    if (best->isFloating())
        Desktop::windowState()->raise(best);
    Desktop::focusState()->fullWindowFocus(best, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
    return true;
}

} // namespace NAwesome
