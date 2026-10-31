#include "../core/bounded.hpp"

#include "harness.hpp"

using namespace NAwesome;

bool test_bounded() {
    // clipToUtf8Boundary: cut inside a 4-byte sequence drops it whole
    {
        std::string s = "ab" "\xF0\x9F\x98\x80" "cd"; // 2 + 4 + 2 = 8
        AW_CHECK(clipToUtf8Boundary(s, 8) == 8);
        AW_CHECK(clipToUtf8Boundary(s, 4) == 2); // mid-emoji
        AW_CHECK(clipToUtf8Boundary(s, 6) == 6); // exact boundary
        AW_CHECK(clipToUtf8Boundary(s, 0) == 0);
        // a 3-byte sequence: landing exactly on its end keeps it, cutting
        // inside drops it
        std::string e = "ab" "\xC3\xA9" "c"; // 5
        AW_CHECK(clipToUtf8Boundary(e, 4) == 4);
        AW_CHECK(clipToUtf8Boundary(e, 3) == 2);
        // a sequence truncated at the input's own end: kept whole when it
        // fits (the input's own invalidity is preserved), dropped when the
        // cap cuts it
        std::string t = "a" "\xF0\x9F\x98"; // 1 + 3 of 4
        AW_CHECK(clipToUtf8Boundary(t, 5) == 4);
        AW_CHECK(clipToUtf8Boundary(t, 3) == 1);
        // all-continuation input clips to nothing
        AW_CHECK(clipToUtf8Boundary(std::string{"\x80\x80\x80"}, 2) == 0);
    }
    return true;
}
