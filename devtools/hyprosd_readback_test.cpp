#include "hyprosd/wpctl.hpp"
#include "test.hpp"

int main() {
    NHyprTest::CSuite suite{"hyprosd_readback_test"};
    const auto        normal = NHyprosd::Wpctl::parseReadback("Volume: 0.56\n");
    suite.expect(normal && normal->value == 0.56 && !normal->muted, "normal wpctl output");

    const auto muted = NHyprosd::Wpctl::parseReadback("  Volume: 1.00 [MUTED]\n");
    suite.expect(muted && muted->value == 1.0 && muted->muted, "muted wpctl output");

    const auto localized = NHyprosd::Wpctl::parseReadback("Volume: 0,25\n");
    suite.expect(localized && localized->value == 0.25, "comma decimal output");

    suite.expect(!NHyprosd::Wpctl::parseReadback("Volume: NaN\n"), "non-finite value rejected");
    suite.expect(!NHyprosd::Wpctl::parseReadback("Volume: 0.5 diagnostic\n"), "trailing diagnostic rejected");
    suite.expect(!NHyprosd::Wpctl::parseReadback("failed to connect\n"), "diagnostic-only output rejected");

    return suite.finish();
}
