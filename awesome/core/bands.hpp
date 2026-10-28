// awesome/core/bands.hpp — a band laid across a monitor right under the
// bar: the shell's launcher strip (awesome's menubar wibox) while it is
// open. The notify column's cards stack under it (offset_y clears the bar
// alone), or a banner would cover the strip's matches. The shell publishes
// the band as the strip opens and shuts; notify reads it in cardsTop and
// restacks off the hook. Neither module reads the other's state.
//
// Main thread only. The shell's teardown shuts the strip (the band goes
// with it); notify's clears its hook.
#pragma once

#include <hyprland/src/SharedDefs.hpp>

#include <functional>

namespace NAwesome::Bands {

    struct SBand {
        MONITORID mon = -1;
        double    h   = 0; // logical px under the bar; 0 while shut

        bool      operator==(const SBand&) const = default;
    };
    inline SBand& launcher() {
        static SBand B;
        return B;
    }

    // the reader's: runs after the band moved (the cards restack)
    inline std::function<void()>& changedHook() {
        static std::function<void()> H;
        return H;
    }

    inline void publishLauncher(SBand b) {
        if (launcher() == b)
            return;
        launcher() = b;
        if (changedHook())
            changedHook()();
    }

    // how far the bands reach under the bar on that monitor
    inline double below(MONITORID mon) {
        const auto& B = launcher();
        return B.mon == mon ? B.h : 0;
    }

} // namespace NAwesome::Bands
