// awesome/test/test_wpctl.cpp — the readback parse: the single consumer
// of `wpctl get-volume` output.
#include "../system/wpctl.hpp"

#include "harness.hpp"

bool test_wpctl() {
    using NAwesome::System::Wpctl::parseReadback;

    // the common shapes
    {
        const auto R = parseReadback("Volume: 0.85");
        AW_CHECK(R.has_value());
        AW_CHECK(std::abs(R->value - 0.85) < 1e-9);
        AW_CHECK(!R->muted);
    }
    {
        const auto R = parseReadback("Volume: 1.00");
        AW_CHECK(R.has_value());
        AW_CHECK(R->value >= 1.0);
        AW_CHECK(!R->muted);
    }
    {
        const auto R = parseReadback("Volume: 0.00 [MUTED]");
        AW_CHECK(R.has_value());
        AW_CHECK(R->muted);
        AW_CHECK(R->value == 0.0);
    }
    {
        const auto R = parseReadback("  Volume: 0.50 [MUTED]\n");
        AW_CHECK(R.has_value());
        AW_CHECK(R->muted);
        AW_CHECK(std::abs(R->value - 0.50) < 1e-9);
    }
    // the locale decimal comma (one comma, no dot)
    {
        const auto R = parseReadback("Volume: 0,85");
        AW_CHECK(R.has_value());
        AW_CHECK(std::abs(R->value - 0.85) < 1e-9);
    }
    // hostility: nothing but a number and the [MUTED] flag is valid
    AW_CHECK(!parseReadback("").has_value());
    AW_CHECK(!parseReadback("Volume:").has_value());
    AW_CHECK(!parseReadback("Volume: abc").has_value());
    AW_CHECK(!parseReadback("Volume: -0.5").has_value()); // negative
    AW_CHECK(!parseReadback("Volume: 1e400").has_value()); // overflow to inf
    AW_CHECK(!parseReadback("Volume: 0.5 garbage").has_value());
    AW_CHECK(!parseReadback("Volume: 0.5 [MUTED] junk").has_value());
    AW_CHECK(!parseReadback("Volume: 0,.5").has_value()); // two separators
    AW_CHECK(!parseReadback("No device found").has_value()); // no default sink
    AW_CHECK(!parseReadback("").has_value());
    return true;
}
