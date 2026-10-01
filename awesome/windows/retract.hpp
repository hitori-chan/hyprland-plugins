// awesome/windows/retract.hpp — the map-focus retraction: a window that
// re-appears (tray return) and asks for attention keeps the urgency, not
// the focus.
//
// A tray-returning app re-maps as a brand-new window, and a new map takes
// initial focus (FOCUS_REASON_NEW_WINDOW) — the same path that focuses the
// terminal you just spawned; the compositor cannot tell the two apart
// (awesome has the same property: its global focus rule focuses tray-return
// clients, and the classic fix is a per-app focus=false rule). The
// discriminator is what follows the map: the app ASKS for attention
// (an xdg-activation or X11 ping; with focus_on_activate off the ask is
// urgency, and the fork marks the ask urgent EVEN when the window already
// holds the map focus — without that the retraction could never see the
// tray-return burst, the exact case where the focus was taken). When that
// ask lands on a window that (a) holds the focus its own
// fresh map just took, (b) mapped within the arrival window below, and
// (c) has a live pre-arrival focus target, and (d) was not triggered by
// the user's own tray click (the stamp in core/activate.hpp: the SNI
// Activate makes the app re-map and ask itself, byte-identical to a
// message return), the focus goes back to the pre-arrival window —
// raised, so the newcomer doesn't sit on top of your work — and the
// urgency stays (the chip's tint is the whole answer). An app you
// launched that never asks keeps the map focus.
//
// Gate state: with focus_on_activate ON the user has opted into app
// activation focus; the map focus + the activation focus are then the
// intended outcome and nothing is retracted. This module only completes
// the promise the gate-OFF mode makes for the ACTIVATION path, onto the
// MAP path.

#pragma once

#include "state.hpp" // Tasklist::raiseAndFocus, the module's focus path

#include "core/activate.hpp" // userGestureAt: the tray-click suppression stamp
#include "core/hop.hpp"
#include "core/queries.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

#include <chrono>
#include <unordered_map>

namespace NAwesome::Windows::Retract {

    // map -> activation ask is a back-to-back protocol burst (the client
    // commits the map and requests activation in the same round); the
    // window only needs to cover event batching + frame scheduling. Short
    // enough that a launched app pinging seconds later is not a return.
    inline constexpr std::chrono::steady_clock::duration kArrivalWindow = std::chrono::seconds(2);

    // raw-pointer keyed (like the arrival order), dropped in forget() on
    // destroy before the address can be reused; bounded by the window list.
    inline std::unordered_map<const void*, std::chrono::steady_clock::time_point>& arrivals() {
        static std::unordered_map<const void*, std::chrono::steady_clock::time_point> M;
        return M;
    }
    // (prev, last) focus pair: the return target is whoever held focus
    // BEFORE the newcomer's map took it. Updated from window.active.
    inline PHLWINDOWREF& prevFocus() {
        static PHLWINDOWREF R;
        return R;
    }
    inline PHLWINDOWREF& lastFocus() {
        static PHLWINDOWREF R;
        return R;
    }
    inline CHop& pending() {
        static CHop H;
        return H;
    }

    // window.open: the moment a window becomes a "new window" to the
    // compositor (and takes, or is offered, initial focus).
    inline void noteArrival(const PHLWINDOW& w) {
        if (w)
            arrivals()[w.get()] = std::chrono::steady_clock::now();
    }

    // window.active: every focus change shifts the pair (w may be null —
    // focus went to the desktop — an empty ref is the "nowhere" target).
    inline void noteFocus(const PHLWINDOW& w) {
        prevFocus()  = lastFocus();
        lastFocus()  = PHLWINDOWREF{w};
    }

    // window.urgent: the attention ask. Only a fresh map that is holding
    // the focus it just took gets retracted; everything else (a ping on an
    // established window, a focused-but-old window, no pre-arrival target,
    // gate on, locked session) is left alone.
    inline void maybeRetract(const PHLWINDOW& w) {
        static auto FOCUS_ON_ACTIVATE = CConfigValue<Config::INTEGER>("misc:focus_on_activate");
        if (!w || !w->mapped() || *FOCUS_ON_ACTIVATE || sessionLocked())
            return;
        // A tray click inside the grace made the app re-map and ask for
        // itself: the burst is the user's, keep the map focus.
        if (std::chrono::steady_clock::now() - userGestureAt() < kArrivalWindow)
            return;
        if (!Desktop::focusState() || Desktop::focusState()->window() != w)
            return;
        const auto IT = arrivals().find(w.get());
        if (IT == arrivals().end())
            return;
        if (std::chrono::steady_clock::now() - IT->second > kArrivalWindow)
            return;
        const auto P = prevFocus().lock();
        if (!P || !P->mapped() || P->isHidden())
            return; // nowhere to go back to: the newcomer keeps the focus
        // Deferred like every other focus change out of an emission; the
        // whole condition is re-checked at the hop, because focus may have
        // moved (a click) between the ask and the safe point.
        pending().arm([WR = PHLWINDOWREF{w}, PR = PHLWINDOWREF{P}]() {
            if (sessionLocked())
                return;
            const auto W = WR.lock();
            const auto P = PR.lock();
            if (!W || !P || !P->mapped() || P->isHidden())
                return;
            if (W->isHidden() || !W->mapped())
                return;
            if (!Desktop::focusState() || Desktop::focusState()->window() != W)
                return;
            Tasklist::raiseAndFocus(P); // raise (the newcomer came on top) + focus
        });
    }

    inline void forget(const void* w) {
        arrivals().erase(w);
    }

    inline void exit() {
        pending().reset();
        arrivals().clear();
        prevFocus()  = PHLWINDOWREF{};
        lastFocus()  = PHLWINDOWREF{};
    }

} // namespace NAwesome::Windows::Retract
