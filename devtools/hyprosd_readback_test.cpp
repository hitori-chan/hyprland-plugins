#include "hyprosd/wpctl.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

    int failures = 0;

    void expect(bool condition, std::string_view name) {
        if (condition)
            return;
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }

} // namespace

int main() {
    const auto normal = NHyprosd::Wpctl::parseReadback("Volume: 0.56\n");
    expect(normal && normal->value == 0.56 && !normal->muted, "normal wpctl output");

    const auto muted = NHyprosd::Wpctl::parseReadback("  Volume: 1.00 [MUTED]\n");
    expect(muted && muted->value == 1.0 && muted->muted, "muted wpctl output");

    const auto localized = NHyprosd::Wpctl::parseReadback("Volume: 0,25\n");
    expect(localized && localized->value == 0.25, "comma decimal output");

    expect(!NHyprosd::Wpctl::parseReadback("Volume: NaN\n"), "non-finite value rejected");
    expect(!NHyprosd::Wpctl::parseReadback("Volume: 0.5 diagnostic\n"), "trailing diagnostic rejected");
    expect(!NHyprosd::Wpctl::parseReadback("failed to connect\n"), "diagnostic-only output rejected");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "hyprosd_readback_test: all checks passed\n";
}
