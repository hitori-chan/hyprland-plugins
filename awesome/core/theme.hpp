// awesome/core/theme.hpp — the Frost material at runtime: the color memo,
// the fixed fills as ready colors, and the compositor gates the glass
// rides on. The tokens themselves live in tokens.hpp (the schema's
// defaults name them; theme.lua overrides those through the schema).
#pragma once

#include "schema.hpp"
#include "tokens.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

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

    // a hover's step for an ink already at full strength (the accent, the
    // urgent red): toward white, its alpha kept. Not per draw call: the
    // ctor converts to OkLab (the bar lifts its palette once a frame).
    inline CHyprColor lifted(const CHyprColor& c) {
        constexpr double K = 0.4;
        return CHyprColor{(float)(c.r + (1 - c.r) * K), (float)(c.g + (1 - c.g) * K), (float)(c.b + (1 - c.b) * K), (float)c.a};
    }

    // the SP form: the schema's color values are held as ConfigValue handles
    inline CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        return color(v->value());
    }

    // static fills, constructed once each
#define AW_THEME_COLOR(NAME, TOKEN)                                                                                                                                       \
    inline const CHyprColor& NAME() {                                                                                                                                     \
        static const CHyprColor C{Theme::TOKEN};                                                                                                                          \
        return C;                                                                                                                                                         \
    }
    AW_THEME_COLOR(tSub, SUB)
    AW_THEME_COLOR(tFill, FILL)
    AW_THEME_COLOR(tFillHover, FILL_HOVER)
    AW_THEME_COLOR(tTrack, TRACK)
    AW_THEME_COLOR(tHairline, HAIRLINE)
    AW_THEME_COLOR(tWell, WELL)
    AW_THEME_COLOR(tWellRim, WELL_RIM)
    AW_THEME_COLOR(tControl, CONTROL)
    AW_THEME_COLOR(tSendArrow, SEND_ARROW)
    AW_THEME_COLOR(tLight, LIGHT)
    AW_THEME_COLOR(tTitle, TITLE)
    AW_THEME_COLOR(tShadow, SHADOW)
    AW_THEME_COLOR(tMenuShadow, MENU_SHADOW)
    AW_THEME_COLOR(tOnAccent, ON_ACCENT)
    AW_THEME_COLOR(tLitInk, LIT_INK)
    AW_THEME_COLOR(tBadgeRim, BADGE_RIM)
    AW_THEME_COLOR(tBadgeGround, BADGE_GROUND)
#undef AW_THEME_COLOR

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
