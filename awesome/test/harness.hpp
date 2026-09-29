// awesome/test/harness.hpp — the headless test harness. No external
// dependencies (the build links against distro packages only); one CHECK
// macro, one failure counter, a per-case table in main.
#pragma once

#include <cstdio>

namespace NTest {
    struct Case {
        const char* name;
        bool (*fn)();
    };
    inline int& fails() {
        static int F = 0;
        return F;
    }
    inline bool check(bool ok, const char* expr, const char* file, int line) {
        if (!ok) {
            std::printf("    FAIL %s:%d  %s\n", file, line, expr);
            ++fails();
        }
        return ok;
    }
} // namespace NTest

#define AW_CHECK(expr) NTest::check((expr), #expr, __FILE__, __LINE__)
