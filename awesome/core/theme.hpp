// awesome/core/theme.hpp — the glass·ink material: the C++ config DEFAULTS,
// which theme.lua overrides at runtime through the schema, plus the runtime
// color memo and the compositor gates the glass rides on. Ported from
// common/theme.hpp + common/glass.hpp (merged: the tokens and the memo are
// one concern now that one plugin owns all the drawing).
#pragma once

#include "schema.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace NAwesome::Theme {

    // ---- material (0xAARRGGBB, config default form) ----
    inline constexpr uint32_t GLASS      = 0x9e0f1218; // panel/island fill, 62% graphite
    inline constexpr uint32_t INK        = 0xffe4e8ee; // primary text
    inline constexpr uint32_t TITLE      = 0xffeef1f5; // card titles / emphasis
    inline constexpr uint32_t SUB        = 0xff98a2ac; // secondary text
    inline constexpr uint32_t ACCENT     = 0xff32d6ff; // heritage cyan
    inline constexpr uint32_t ACCENT_DIM = 0x2932d6ff; // accent @16%
    inline constexpr uint32_t ON_ACCENT  = 0xff07161c; // text over an accent fill
    inline constexpr uint32_t URGENT     = 0xffff8a5c; // critical / urgent
    inline constexpr uint32_t LINK       = 0xff7db4ff; // body hyperlinks
    inline constexpr uint32_t FILL       = 0x0bffffff; // white @4.5%
    inline constexpr uint32_t FILL2      = 0x17ffffff; // white @9%
    inline constexpr uint32_t LINE       = 0x17dcebff; // hairlines @9%
    inline constexpr uint32_t SHADOW     = 0x73000000; // card shadow ink @45%
    inline constexpr uint32_t BADGE_RIM  = 0xfff4f6f8; // the identity badge's disc

    // ---- the bar roles ----
    inline constexpr uint32_t PANEL            = 0xff132732;
    inline constexpr uint32_t ON_SURFACE       = 0xffeef3f5;
    inline constexpr uint32_t ON_SURFACE_VARIANT = 0xffd1dde1;
    inline constexpr uint32_t PRIMARY          = 0xff9acbff;
    inline constexpr uint32_t STATE            = 0x339acbff;
    inline constexpr uint32_t ON_PRIMARY       = 0xff102333;
    inline constexpr uint32_t ON_SURFACE_DISABLED = 0x61d1dde1;
    inline constexpr uint32_t ERROR            = 0xffffb4ab;
    inline constexpr uint32_t ERROR_CONTAINER  = 0xff93000a;
    inline constexpr uint32_t OUTLINE          = 0x33e0f0f8;

    inline constexpr const char* FONT          = "IBM Plex Sans";
    inline constexpr int         RAD_CARD      = 16;
    inline constexpr int         RAD_ROW       = 14; // the shell's menu rows: no rounding config, a fixed pill
    inline constexpr double      ROUNDING_POWER = 3.0;

    // ---- motion (ms) ----
    inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival

} // namespace NAwesome::Theme

namespace NAwesome {

    // a config color, converted once per value change (main thread only).
    // CHyprColor's uint64 ctor OkLab-converts: constants must never be
    // constructed per draw call.
    inline CHyprColor color(uint64_t raw) {
        struct SMemo {
            uint64_t   key = 0;
            bool       set = false;
            CHyprColor col;
        };
        static std::unordered_map<uint64_t, SMemo> memo;
        auto&                                        M = memo[raw];
        if (!M.set || M.key != raw) {
            M.key = raw;
            M.set = true;
            M.col = CHyprColor{raw};
        }
        return M.col;
    }

    // the SP form: the schema's color values are held as ConfigValue handles
    inline CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        return color(v->value());
    }

    // static fills, constructed once each
    inline const CHyprColor& tFill() {
        static const CHyprColor C{Theme::FILL};
        return C;
    }
    inline const CHyprColor& tFill2() {
        static const CHyprColor C{Theme::FILL2};
        return C;
    }
    inline const CHyprColor& tAccentDim() {
        static const CHyprColor C{Theme::ACCENT_DIM};
        return C;
    }
    inline const CHyprColor& tLine() {
        static const CHyprColor C{Theme::LINE};
        return C;
    }
    inline const CHyprColor& tOnAccent() {
        static const CHyprColor C{Theme::ON_ACCENT};
        return C;
    }

    // animations=0 is the shell's motion kill switch
    inline bool animationsOn() {
        static auto V = CConfigValue<Config::INTEGER>("animations:enabled");
        return *V != 0;
    }

    // the glass samples the compositor's decoration blur; damage must grow
    // by its radius wherever a glass surface paints
    inline bool blurOn() {
        static auto V = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
        return *V != 0;
    }
    inline double blurRadius() {
        if (!blurOn())
            return 0;
        static auto SIZE   = CConfigValue<Config::INTEGER>("decoration:blur:size");
        static auto PASSES = CConfigValue<Config::INTEGER>("decoration:blur:passes");
        return (double)*SIZE * (1 << std::clamp((int)*PASSES, 1, 6));
    }

} // namespace NAwesome
