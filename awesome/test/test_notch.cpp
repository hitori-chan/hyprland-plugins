// awesome/test/test_notch.cpp — core/notch.hpp: the band's height read from
// a device tree (the enabled display node's big-endian u32, a disabled one
// skipped, none on a machine without one) and the notch's geometry: shown
// only for the 16:10 panel plus exactly the band, centred, in logical px.
#include "../core/notch.hpp"

#include "harness.hpp"

#include <cstring>
#include <unistd.h>

using namespace NAwesome::Notch;

namespace {
    void writeU32(const std::filesystem::path& p, uint32_t v) {
        std::ofstream F{p, std::ios::binary};
        const unsigned char B[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
        F.write(reinterpret_cast<const char*>(B), 4);
    }
    void writeStatus(const std::filesystem::path& p, const char* s) {
        std::ofstream F{p, std::ios::binary};
        F.write(s, (std::streamsize)std::strlen(s) + 1); // a DT string ends in NUL
    }
}

bool test_notch() {
    const auto DT = std::filesystem::temp_directory_path() / ("awesome-test-dt-" + std::to_string(getpid()));
    std::filesystem::remove_all(DT);
    AW_CHECK(bandHeightPx(DT) == 0); // no device tree (a PC)

    std::filesystem::create_directories(DT / "soc" / "dcp@386c00000");
    std::filesystem::create_directories(DT / "soc" / "dcp@38bc00000");
    AW_CHECK(bandHeightPx(DT) == 0); // nodes without the property

    // the disabled node's value never counts; the enabled one's does
    writeStatus(DT / "soc" / "dcp@38bc00000" / "status", "disabled");
    writeU32(DT / "soc" / "dcp@38bc00000" / "apple,notch-height", 99);
    AW_CHECK(bandHeightPx(DT) == 0);
    writeU32(DT / "soc" / "dcp@386c00000" / "apple,notch-height", 74);
    AW_CHECK(bandHeightPx(DT) == 74);
    writeStatus(DT / "soc" / "dcp@386c00000" / "status", "okay");
    AW_CHECK(bandHeightPx(DT) == 74);
    std::filesystem::remove_all(DT);

    // the 14" panel with the notch shown: 3024x1964 at 2, the band 37
    // logical, centred; the estimate's width, or the configured one
    {
        const auto N = geometry(74, 3024, 1964, 2.0);
        AW_CHECK(N.has_value());
        AW_CHECK(N->h == 37);
        AW_CHECK(N->w == std::round(74 * NOTCH_ASPECT) / 2.0);
        AW_CHECK(std::abs(N->x + N->w / 2 - 756) < 1e-9);
        const auto C = geometry(74, 3024, 1964, 2.0, 200);
        AW_CHECK(C && C->w == 200 && C->x == 656);
    }
    AW_CHECK(!geometry(74, 3024, 1890, 2.0));  // the notch hidden: the 16:10 mode starts below it
    AW_CHECK(!geometry(0, 3024, 1964, 2.0));   // no band in the firmware
    AW_CHECK(!geometry(74, 1920, 1080, 1.0));  // another panel
    AW_CHECK(!geometry(74, 1964, 3024, 2.0));  // rotated
    AW_CHECK(geometry(74, 3456, 2234, 2.0).has_value()); // the 16" panel
    return true;
}
