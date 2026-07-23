// common/glass.hpp — the RUNTIME side of theme.hpp, shared by the drawing
// plugins: memoized theme colors (CHyprColor's uint64 ctor OkLab-converts,
// so constants must never be constructed per draw call), the memoized
// config-color fetch, and the compositor gates the glass rides on (the
// decoration blur it samples, the animations kill switch the motion system
// honors).
#pragma once

#include "theme.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <algorithm>
#include <unordered_map>

namespace NHyprCommon {

    // a config color, converted once per value change (main thread only)
    inline CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        struct SMemo {
            uint64_t   raw = 0;
            bool       set = false;
            CHyprColor col;
        };
        static std::unordered_map<const void*, SMemo> memo;
        auto&                                         M = memo[v.get()];
        if (!M.set || M.raw != (uint64_t)v->value()) {
            M.raw = (uint64_t)v->value();
            M.set = true;
            M.col = CHyprColor{M.raw};
        }
        return M.col;
    }

    // the theme fills, converted exactly once
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

} // namespace NHyprCommon
