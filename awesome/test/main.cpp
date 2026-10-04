// awesome/test/main.cpp — the runner.
#include "harness.hpp"
#include "tests.hpp"

int main() {
    const NTest::Case CASES[] = {
        {"bounded", test_bounded},
        {"box", test_box},
        {"store", test_store},
        {"schema", test_schema},
        {"wpctl", test_wpctl},
        {"desktop-exec", test_desktop_exec},
        {"proc", test_proc},
    };
    for (const auto& C : CASES)
        std::printf("[case] %s\n", C.name), C.fn();
    if (NTest::fails()) {
        std::printf("== awesome-test: %d FAILURES ==\n", NTest::fails());
        return 1;
    }
    std::printf("== awesome-test: ALL %zu CASES PASSED ==\n", sizeof(CASES) / sizeof(CASES[0]));
    return 0;
}
