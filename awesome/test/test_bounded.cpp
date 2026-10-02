// awesome/test/test_bounded.cpp
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
    // BoundedString: display clip is UTF-8 safe
    {
        BoundedString<4> B;
        AW_CHECK(!B.assignClipped("h\xc3\xa9llo")); // 6 bytes
        AW_CHECK(B.clipped());
        // 4 bytes = h + é + l
        AW_CHECK(B.view() == std::string_view{"h\xc3\xa9l"});
        BoundedString<16> S;
        AW_CHECK(S.assignStrict("opaque-id"));
        AW_CHECK(!S.clipped() && !S.rejected());
        AW_CHECK(!S.assignStrict("0123456789abcdefg")); // 17 > 16
        AW_CHECK(S.rejected() && !S.str().size());
        // strict after clip: the flags reset per admission
        BoundedString<4> C;
        C.assignClipped("abcdefgh");
        AW_CHECK(C.assignStrict("abcd"));
        AW_CHECK(!C.clipped() && !C.rejected());
    }
    // BoundedQueue: reject and evict at the bound
    {
        BoundedQueue<int, 2> Q;
        AW_CHECK(Q.tryPush(1) && Q.tryPush(2) && !Q.tryPush(3));
        AW_CHECK(Q.size() == 2 && Q.full());
        const auto EV = Q.pushEvict(4);
        AW_CHECK(EV == 1 && Q.size() == 2 && Q.front() == 2 && Q.back() == 4);
        int out = 0;
        AW_CHECK(Q.pop(out) && out == 2);
        AW_CHECK(Q.tryPush(9)); // room after the pop
        AW_CHECK(Q.size() == 2 && Q.front() == 4 && Q.back() == 9);
        Q.clear();
        AW_CHECK(Q.empty());
    }
    return true;
}
