// awesome/core/box.hpp — the plugin's own geometry type. The fork's CBox
// conversion happens at the module edges; core and the headless tests are
// fork-header-free.
//
// Admission: coordinates and sizes
// are integers strictly inside llround's representable domain (hostile
// state must not invoke undefined behavior on the write path), sizes
// non-negative.
#pragma once

#include <cstdint>

namespace NAwesome {

    struct Box {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        constexpr bool operator==(const Box&) const = default;
    };

    // 2^30: comfortably inside the long long domain llround() maps to,
    // far outside any monitor, and a hostile 32-bit parse can't overflow
    // past it.
    inline constexpr int MAX_COORD   = 1 << 30;
    inline constexpr int MIN_COORD   = -(1 << 30);

    inline constexpr bool validBoxNumber(int v) {
        return v >= MIN_COORD && v <= MAX_COORD;
    }

    inline constexpr bool validBox(const Box& b) {
        return validBoxNumber(b.x) && validBoxNumber(b.y) && validBoxNumber(b.w) && validBoxNumber(b.h) && b.w >= 0 && b.h >= 0;
    }

} // namespace NAwesome
