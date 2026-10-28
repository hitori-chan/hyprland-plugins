// awesome/core/bounded.hpp — the one cut every externally sized string
// takes: a byte cap that never splits a UTF-8 sequence.
#pragma once

#include <cstddef>
#include <string_view>

namespace NAwesome {

    // Largest prefix of s of at most cap bytes that ends on a codepoint
    // boundary (the contract is over well-formed UTF-8; malformed input is
    // preserved up to the last lead position, never extended). Walk back
    // from the cap over continuation bytes: the lead position n is the last
    // boundary before the sequence the cap cuts, whether that sequence is
    // complete or truncated at the input's end — the lead byte itself is
    // outside the prefix, so nothing is half-emitted.
    inline size_t clipToUtf8Boundary(std::string_view s, size_t cap) {
        if (s.size() <= cap)
            return s.size();
        size_t n = cap;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        return n;
    }

} // namespace NAwesome
