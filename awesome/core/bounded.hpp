// The one cut every externally sized string takes: a byte cap that never
// splits a UTF-8 sequence.
#pragma once

#include <cstddef>
#include <string_view>

namespace NAwesome {

    // Largest prefix of s of at most cap bytes that ends on a codepoint
    // boundary; malformed UTF-8 is kept up to the last lead position, never
    // extended.
    inline size_t clipToUtf8Boundary(std::string_view s, size_t cap) {
        if (s.size() <= cap)
            return s.size();
        size_t n = cap;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        return n;
    }

} // namespace NAwesome
