// common/input.hpp — the pointer-button policy shared by input listeners.
#pragma once

#include <linux/input-event-codes.h>

#include <cstdint>

namespace NHyprCommon {

    // Only buttons with an action in the shell may be swallowed. Other
    // buttons remain native application input and must never share a mask.
    inline uint32_t trackedPointerButtonBit(uint32_t button) noexcept {
        switch (button) {
            case BTN_LEFT: return 1u;
            case BTN_RIGHT: return 2u;
            case BTN_MIDDLE: return 4u;
            default: return 0u;
        }
    }

} // namespace NHyprCommon
