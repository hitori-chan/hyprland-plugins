// common/theme.hpp — the glass·ink token block, the ONE theme every drawing
// plugin sources (hyprbar + hyprnotify fully, hyprsnap its accent). These are
// the C++ config DEFAULTS; theme.lua overrides them at runtime through the
// same plugin config values as always. A future re-theme is a swap of this
// block, never a code path.
//
// Values are the decided contract (2026-07-23): graphite frosted glass
// #0f1218 @ 62%, accent #32d6ff, urgent #ff8a5c, IBM Plex Sans, radii
// panel 22 / card 16 / row 14, rounding power 3.
#pragma once

#include <cstdint>

namespace NHyprCommon::Theme {

    // ---- material -------------------------------------------------------
    // 0xAARRGGBB — the format Config::Values::CColorValue defaults take.
    inline constexpr uint64_t GLASS      = 0x9e0f1218; // panel/island fill, 62% graphite
    inline constexpr uint64_t INK        = 0xffe4e8ee; // primary text
    inline constexpr uint64_t TITLE      = 0xffeef1f5; // card titles / emphasis
    inline constexpr uint64_t SUB        = 0xff98a2ac; // secondary text: headers, ages, hints
    inline constexpr uint64_t ACCENT     = 0xff32d6ff; // heritage cyan
    inline constexpr uint64_t ACCENT_DIM = 0x2932d6ff; // accent @16%: hover fills, selections
    inline constexpr uint64_t ON_ACCENT  = 0xff07161c; // text over an accent fill
    inline constexpr uint64_t URGENT     = 0xffff8a5c; // critical / urgent
    inline constexpr uint64_t LINK       = 0xff7db4ff; // body hyperlinks
    inline constexpr uint64_t FILL       = 0x0bffffff; // white @4.5%: resting chips
    inline constexpr uint64_t FILL2      = 0x17ffffff; // white @9%: hover / raised chips
    inline constexpr uint64_t LINE       = 0x17dcebff; // hairlines @9%
    inline constexpr uint64_t SHADOW     = 0x73000000; // card shadow ink @45%

    // ---- type -----------------------------------------------------------
    inline constexpr const char* FONT = "IBM Plex Sans"; // shell UI; Fira Code stays in terminals

    // ---- radii (logical px) ----------------------------------------------
    inline constexpr int    RAD_PANEL      = 22; // center panel
    inline constexpr int    RAD_CARD       = 16; // popup cards
    inline constexpr int    RAD_ROW        = 14; // center rows
    inline constexpr int    RAD_ICON_POPUP = 10; // 44px icon column
    inline constexpr int    RAD_ICON_ROW   = 8;  // 34px row icon
    inline constexpr int    RAD_INNER      = 5;  // segmented group children
    inline constexpr double ROUNDING_POWER = 3.0;

    // ---- motion (ms; every plugin honors animations=0 as the kill switch) -
    inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival
    inline constexpr int MOTION_FAST    = 170; // morphs, folds
    inline constexpr int MOTION_EFFECT  = 150; // color/hover — never overshoots
    inline constexpr int MOTION_DETACH  = 260; // dismiss exit

} // namespace NHyprCommon::Theme
