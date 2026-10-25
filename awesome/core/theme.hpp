// awesome/core/theme.hpp — the Frost material: the C++ config DEFAULTS,
// which theme.lua overrides at runtime through the schema, plus the runtime
// color memo and the compositor gates the glass rides on. The tokens and
// the memo are one concern: one plugin owns all the drawing.
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

    // ---- the three glass thicknesses (0xAARRGGBB, config default form) ----
    inline constexpr uint32_t BAR_GLASS  = 0x6b1c1c20; // thin: the bar
    inline constexpr uint32_t GLASS      = 0x8a26262c; // regular: cards, the center, capsules
    inline constexpr uint32_t MENU_GLASS = 0xad28282e; // thick: menus, the launcher's popovers

    // ---- ink: one white at four strengths, the blue as text only ----
    inline constexpr uint32_t TITLE     = 0xffededf0; // primary
    inline constexpr uint32_t INK       = 0xdbebebf5; // the bar's text, 86 %
    inline constexpr uint32_t BODY      = 0xc7ebebf5; // body text, 78 %
    inline constexpr uint32_t SUB       = 0x8cebebf5; // secondary: meta, kickers, 55 %
    inline constexpr uint32_t DIM       = 0x57ebebf5; // tertiary: empty tags, minimized, 34 %
    inline constexpr uint32_t ACCENT    = 0xff00ccff; // selection and focus — text, never a fill
    inline constexpr uint32_t ON_ACCENT = 0xff00141a; // the bell badge's count (the one accent fill)
    inline constexpr uint32_t URGENT    = 0xffff6159; // critical, urgent, low battery
    inline constexpr uint32_t LINK      = 0xff7db4ff; // body hyperlinks
    inline constexpr uint32_t CHARGING  = 0xff32d74b;
    inline constexpr uint32_t SAVER     = 0xffffd60a;

    // ---- fills over glass ----
    inline constexpr uint32_t FILL        = 0x1fffffff; // buttons, chips, fields: white 12 %
    inline constexpr uint32_t FILL_HOVER  = 0x33ffffff; // the same under the pointer, 20 %
    inline constexpr uint32_t TRACK       = 0x29ffffff; // a progress or level track, 16 %
    inline constexpr uint32_t HAIRLINE    = 0x1affffff; // separators, 10 %
    inline constexpr uint32_t RIM         = 0x1affffff; // the glass edge, 10 %
    inline constexpr uint32_t LIGHT       = 0x1affffff; // the top edge catching light, 10 %
    inline constexpr uint32_t GROUND      = 0x4d000000; // the viewed tag's dark ground, 30 %
    inline constexpr uint32_t GROUND_HOVER = 0x1affffff; // a hovered tag or row, 10 %
    inline constexpr uint32_t SHADOW      = 0x47000000; // a card's drop, 28 %
    inline constexpr uint32_t MENU_SHADOW = 0x6b000000; // a menu floats higher, 42 %
    inline constexpr uint32_t BADGE_RIM   = 0xffffffff; // the identity badge's ring
    inline constexpr uint32_t BADGE_GROUND = 0xff2c2c32; // under its glyph: a symbolic icon stays legible

    // ---- geometry (logical px) ----
    inline constexpr int    RAD_CARD   = 18; // notify:rounding's default
    inline constexpr int    RAD_MENU   = 12;
    inline constexpr int    RAD_ROW    = 6; // menu rows, tags, chips
    inline constexpr int    RAD_BUTTON = 9; // card buttons, app icons
    inline constexpr double ROUNDING_POWER = 3.0;

    inline constexpr const char* FONT = "Inter";

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
    AW_THEME_COLOR(tLight, LIGHT)
    AW_THEME_COLOR(tGround, GROUND)
    AW_THEME_COLOR(tGroundHover, GROUND_HOVER)
    AW_THEME_COLOR(tShadow, SHADOW)
    AW_THEME_COLOR(tMenuShadow, MENU_SHADOW)
    AW_THEME_COLOR(tOnAccent, ON_ACCENT)
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
