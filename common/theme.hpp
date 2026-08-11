// common/theme.hpp — the glass·ink token block: the C++ config DEFAULTS for
// the MATERIAL, which theme.lua then overrides at runtime through the usual
// plugin config values. A re-theme is a swap of this block, never a code
// path.
//
// Who actually sources it, so the next reader does not go looking for more:
// hyprnotify takes its whole palette from here; hyprbar takes the shared
// FILLS and gates through glass.hpp plus two menu radii, while its own
// colour defaults stay the AWESOME palette in its main.cpp — the shell is
// the awesome LAYOUT wearing this MATERIAL, and the bar is where the layout
// half shows. Nothing else reads it.
//
// Values are the decided contract (2026-07-23, restored 2026-08-11): graphite
// frosted glass #0f1218 @ 62%, accent #32d6ff, urgent #ff8a5c, IBM Plex Sans,
// card radius 16, rounding power 3.
#pragma once

#include <cstdint>

namespace NHyprCommon::Theme {

    // ---- material (semantic roles, glass·ink values) --------------------
    // 0xAARRGGBB — the format Config::Values::CColorValue defaults take.
    // The glass·ink palette: graphite frosted glass + heritage cyan accent.
    inline constexpr uint64_t PANEL               = 0x9e0f1218; // panel/island fill, 62% graphite
    inline constexpr uint64_t SURFACE             = 0x9e0f1218; // notification/menu card (same glass)
    inline constexpr uint64_t SURFACE_HIGH        = 0x17ffffff; // controls and raised state (white @9%)
    inline constexpr uint64_t STATE               = 0x2932d6ff; // primary @16% interaction layer
    inline constexpr uint64_t ON_SURFACE          = 0xffe4e8ee; // body text and icons
    inline constexpr uint64_t ON_SURFACE_STRONG   = 0xffeef1f5; // titles and emphasis
    inline constexpr uint64_t ON_SURFACE_VARIANT  = 0xff98a2ac; // metadata and secondary copy
    inline constexpr uint64_t ON_SURFACE_DISABLED = 0x6198a2ac; // disabled content @38%
    inline constexpr uint64_t PRIMARY             = 0xff32d6ff; // heritage cyan: actions, progress, focus
    inline constexpr uint64_t ON_PRIMARY          = 0xff07161c; // content on a primary fill
    inline constexpr uint64_t ERROR               = 0xffff8a5c; // critical content
    inline constexpr uint64_t ON_ERROR            = 0xff2b0900; // content on an error fill
    inline constexpr uint64_t ERROR_CONTAINER     = 0xcc4d1f00; // urgent selected container
    inline constexpr uint64_t OUTLINE             = 0x17dcebff; // hairlines @9%
    inline constexpr uint64_t SHADOW              = 0x73000000; // card shadow ink @45%
    inline constexpr uint64_t BADGE_RIM           = 0xfff4f6f8; // identity badge disc

    // ---- type -----------------------------------------------------------
    inline constexpr const char* FONT = "IBM Plex Sans"; // shell UI; Fira Code stays in terminals

    // ---- radii (logical px) ----------------------------------------------
    // ONE radius is a token. The rest of hyprnotify's family DERIVES from it
    // at runtime (paint.cpp: panel = card + 6, row = card - 2, icons as
    // ratios of their box), so panel 22 / row 14 are facts about the default,
    // not knobs — a second constant holding 22 would be a copy that goes
    // stale the moment plugin:hyprnotify:rounding is set.
    inline constexpr int    RAD_CARD       = 16;
    inline constexpr int    RAD_ROW        = 14; // hyprbar's menu rows, which take no rounding config
    inline constexpr double ROUNDING_POWER = 3.0;

    // ---- motion (ms; every plugin honors animations=0 as the kill switch) -
    inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival

} // namespace NHyprCommon::Theme
