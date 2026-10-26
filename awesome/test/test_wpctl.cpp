// awesome/test/test_wpctl.cpp — the readback parse (the single consumer of
// `wpctl get-volume` output) and the sinks of `wpctl status`.
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

    // the sinks of `wpctl status`: its Audio tree's Sinks branch alone
    {
        using NAwesome::System::Wpctl::parseSinks;
        const auto S = parseSinks("PipeWire 'pipewire-0' [1.4.9, u@h, cookie:1]\n"
                                  " └─ Clients:\n"
                                  "        33. WirePlumber                         [1.4.9, u@h, pid:1]\n"
                                  "\n"
                                  "Audio\n"
                                  " ├─ Devices:\n"
                                  " │      44. Built-in Audio                      [alsa]\n"
                                  " │  \n"
                                  " ├─ Sinks:\n"
                                  " │      53. Built-in Audio Analog Stereo        [vol: 0.35]\n"
                                  " │  *   61. USB Headset: Stereo                 [vol: 1.00 MUTED]\n"
                                  " │  \n"
                                  " ├─ Sources:\n"
                                  " │  *   54. Built-in Audio Analog Stereo        [vol: 0.30]\n"
                                  " │  \n"
                                  " ├─ Filters:\n"
                                  " │  \n"
                                  " └─ Streams:\n"
                                  "\n"
                                  "Video\n"
                                  " ├─ Sinks:\n"
                                  " │      70. Not Audio                           [vol: 1.00]\n"
                                  "\n"
                                  "Settings\n"
                                  " └─ Default Configured Devices:\n"
                                  "         0. Audio/Sink    alsa_output.usb\n");
        AW_CHECK(S.size() == 2);
        AW_CHECK(S[0].id == 53 && S[0].name == "Built-in Audio Analog Stereo" && !S[0].current);
        AW_CHECK(S[1].id == 61 && S[1].name == "USB Headset: Stereo" && S[1].current);
    }
    // no Audio tree, an empty branch, a malformed row
    {
        using NAwesome::System::Wpctl::parseSinks;
        AW_CHECK(parseSinks("").empty());
        AW_CHECK(parseSinks("Audio\n ├─ Sinks:\n │  \n ├─ Sources:\n").empty());
        AW_CHECK(parseSinks("Audio\n ├─ Sinks:\n │  *   x3. Bad\n │      7.\n").empty());
    }
    // bounded: the rows past MAX_SINKS, a name past MAX_SINK_NAME (cut on a code point)
    {
        using namespace NAwesome::System::Wpctl;
        std::string many = "Audio\n ├─ Sinks:\n";
        for (int i = 0; i < 40; i++)
            many += " │      " + std::to_string(100 + i) + ". Sink " + std::to_string(i) + "  [vol: 1.00]\n";
        AW_CHECK(parseSinks(many).size() == MAX_SINKS);
        std::string longName(MAX_SINK_NAME - 1, 'a');
        longName += "é and more";
        const auto L = parseSinks("Audio\n ├─ Sinks:\n │      9. " + longName + "  [vol: 1.00]\n");
        AW_CHECK(L.size() == 1 && L[0].name.size() == MAX_SINK_NAME - 1);
    }
    return true;
}
