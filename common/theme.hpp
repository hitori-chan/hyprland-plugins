// common/theme.hpp — the glass·ink token block: the C++ config DEFAULTS for
// the MATERIAL, which theme.lua then overrides at runtime through the usual
// plugin config values. A re-theme is a swap of this block, never a code
// path.
//
// Who actually sources it, so the next reader does not go looking for more:
// hyprnotify takes its whole palette from here (the island names: GLASS, INK,
// TITLE, SUB, ACCENT, ...); hyprbar takes the shared bar roles (PANEL,
// SURFACE, OUTLINE, ...) plus the menu radii and gates through glass.hpp —
// the shell is the awesome LAYOUT wearing this MATERIAL, and the bar is where
// the layout half shows. Nothing else reads it.
//
// Values are the decided contract (2026-07-23, restored 2026-08-11; island
// names per the v6.9.2 product design, restored 2026-08-30): graphite
// frosted glass #0f1218 @ 62%, heritage cyan accent #32d6ff, urgent #ff8a5c,
// IBM Plex Sans, card radius 16, rounding power 3.
#pragma once

#include <cstdint>

namespace NHyprCommon::Theme {

    // ---- material (the island names; 0xAARRGGBB, CColorValue default form)
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
    inline constexpr uint64_t BADGE_RIM  = 0xfff4f6f8; // the identity badge's disc

    // ---- the bar roles (hyprbar's surface names) --------------------------
    // PANEL/ON_SURFACE/TITLE/SUB/PRIMARY/ON_PRIMARY/URGENT/LINE mirror the
    // island values above under the bar's role names; SURFACE, STATE and
    // OUTLINE keep the bar's own slightly-heavier bakes so the bar's paint
    // stays pixel-stable while the island wears the island names.
    inline constexpr uint64_t PANEL               = 0x9e0f1218; // = GLASS
    inline constexpr uint64_t SURFACE             = 0x0fffffff; // notification/menu card (white @6%, live glass)
    inline constexpr uint64_t SURFACE_HIGH        = 0x1cffffff; // controls and raised state (white @11%)
    inline constexpr uint64_t STATE               = 0x2e32d6ff; // primary @18% interaction layer
    inline constexpr uint64_t ON_SURFACE          = 0xffe4e8ee; // = INK
    inline constexpr uint64_t ON_SURFACE_STRONG   = 0xffeef1f5; // = TITLE
    inline constexpr uint64_t ON_SURFACE_VARIANT  = 0xff98a2ac; // = SUB
    inline constexpr uint64_t ON_SURFACE_DISABLED = 0x6198a2ac; // disabled content @38%
    inline constexpr uint64_t PRIMARY             = 0xff32d6ff; // = ACCENT
    inline constexpr uint64_t ON_PRIMARY          = 0xff07161c; // = ON_ACCENT
    inline constexpr uint64_t ERROR               = 0xffff8a5c; // = URGENT
    inline constexpr uint64_t ON_ERROR            = 0xff2b0900; // content on an error fill
    inline constexpr uint64_t ERROR_CONTAINER     = 0xcc4d1f00; // urgent selected container
    inline constexpr uint64_t OUTLINE             = 0x1fdcebff; // bar hairlines @12%

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
