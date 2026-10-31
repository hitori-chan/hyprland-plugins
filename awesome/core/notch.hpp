// A display notch: the camera housing cut into the top edge of a laptop
// panel (Apple silicon laptops booted with appledrm.show_notch=1, whose mode
// then includes the band on either side of the notch, the 16:10 picture
// below it).
//
// The firmware names the band's height: the device tree's
// apple,notch-height (a big-endian u32, panel px) on the enabled display
// coprocessor node. Nothing names the notch's width, so it comes from
// shell:notch_width, else from the height (NOTCH_ASPECT). The notch shows
// when the mode is the 16:10 panel plus exactly that band (no rotation);
// it is centred.
//
// Pure: no compositor state (the shell binds it to a monitor).
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace NAwesome::Notch {

    // width / band height: the 14" panel's notch measured 370 +- 2 panel px
    // wide on its 74 px band, rounded up to cover the measurement
    inline constexpr double NOTCH_ASPECT = 5.05;

    // the band's height in panel px under a device tree root, 0 = none
    inline int bandHeightPx(const std::filesystem::path& dt) {
        std::error_code ec;
        for (const auto& E : std::filesystem::directory_iterator(dt / "soc", ec)) {
            if (!E.path().filename().string().starts_with("dcp@"))
                continue;
            // a node with a status other than "okay" is disabled
            if (std::ifstream S{E.path() / "status"}; S) {
                std::string status;
                std::getline(S, status, '\0');
                if (status != "okay")
                    continue;
            }
            std::ifstream F{E.path() / "apple,notch-height", std::ios::binary};
            unsigned char B[4];
            if (!F.read(reinterpret_cast<char*>(B), 4))
                continue;
            const uint32_t V = (uint32_t)B[0] << 24 | (uint32_t)B[1] << 16 | (uint32_t)B[2] << 8 | B[3];
            if (V > 0 && V < 512)
                return (int)V;
        }
        return 0;
    }

    struct SNotch {
        double x = 0, w = 0, h = 0; // logical px from the monitor's left and top
    };

    // a modeW x modeH (panel px) mode at scale; widthLogical 0 = estimated
    inline std::optional<SNotch> geometry(int bandPx, int modeW, int modeH, double scale, double widthLogical = 0) {
        if (bandPx <= 0 || modeW <= 0 || scale <= 0 || (modeW * 10) % 16 != 0 || modeH - bandPx != modeW * 10 / 16)
            return std::nullopt;
        const double W = widthLogical > 0 ? widthLogical : std::round(bandPx * NOTCH_ASPECT) / scale;
        return SNotch{.x = (modeW / scale - W) / 2, .w = W, .h = bandPx / scale};
    }

} // namespace NAwesome::Notch
