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
// frosted glass #0f1218 @ 62%, lifted card glass at 6%, accent #32d6ff,
// urgent #ff8a5c, IBM Plex Sans, card radius 16, rounding power 3.
#pragma once

#include <cstdint>

namespace NHyprCommon::Theme {

    // ---- material (semantic roles, glass·ink values) --------------------
    // 0xAARRGGBB — the format Config::Values::CColorValue defaults take.
    // The glass·ink palette: graphite frosted glass + heritage cyan accent.
    inline constexpr uint64_t PANEL               = 0x9e0f1218; // panel/island fill, 62% graphite
    inline constexpr uint64_t SURFACE             = 0x0fffffff; // notification/menu card (white @6%, live glass)
    inline constexpr uint64_t SURFACE_HIGH        = 0x1cffffff; // controls and raised state (white @11%)
    inline constexpr uint64_t STATE               = 0x2e32d6ff; // primary @18% interaction layer
    inline constexpr uint64_t ON_SURFACE          = 0xffe4e8ee; // body text and icons
    inline constexpr uint64_t ON_SURFACE_STRONG   = 0xffeef1f5; // titles and emphasis
    inline constexpr uint64_t ON_SURFACE_VARIANT  = 0xff98a2ac; // metadata and secondary copy
    inline constexpr uint64_t ON_SURFACE_DISABLED = 0x6198a2ac; // disabled content @38%
    inline constexpr uint64_t PRIMARY             = 0xff32d6ff; // heritage cyan: actions, progress, focus
    inline constexpr uint64_t ON_PRIMARY          = 0xff07161c; // content on a primary fill
    inline constexpr uint64_t ERROR               = 0xffff8a5c; // critical content
    inline constexpr uint64_t ON_ERROR            = 0xff2b0900; // content on an error fill
    inline constexpr uint64_t ERROR_CONTAINER     = 0xcc4d1f00; // urgent selected container
    inline constexpr uint64_t OUTLINE             = 0x1fdcebff; // hairlines @12%
    inline constexpr uint64_t SHADOW              = 0x73000000; // card shadow ink @45%
    inline constexpr uint64_t BADGE_RIM           = 0xfff4f6f8; // identity badge disc

    // ---- v13 (2026-08-18 ROM capture) material token sets --------------
    // hyprnotify v13 ships three approved materials for the shade: M · tray
    // (the hyprbar tray-menu material, OPAQUE — the default), A · ink (the
    // 2026-08-18 ROM captures' own tones) and E · glass (the tray-menu
    // frost). Each struct holds the roles the plugin cannot derive from the
    // shared roles above; `theme` in main.cpp maps the config option to one
    // set, and hyprnotify's ui.hpp resolves every colour role as "follow
    // the set while the config value still holds the INK default" — an
    // explicit user colour always wins.
    // Sources: hyprbar/menu.cpp (the tray menu's render),
    // docs/demos/hyprnotify-design-mixer-v13/style.css (.glass.vA + shared
    // .glass) and the 2026-08-18 device captures.
    struct SV13 {
        uint64_t panel, card, heads; // container veils; heads = the HUN's own heavier veil
        uint64_t raised, raisedH;    // rest fill; hover / pressed fill
        uint64_t rowLine, rim;       // hairlines / dividers; the panel rim
        uint64_t selRow, selRowB;    // hold-menu selected row fill / outline (0 = none)
        uint64_t on, on82, on60, on40; // the text ramp
        uint64_t action;             // action-row text (A splits it from the accent)
        uint64_t onAccent;           // content on the accent fill
        uint64_t chip;               // the expand/count stadium fill
        uint64_t pillBg, pillFg;     // shade count pill (number + chevron)
        uint64_t headPillBg, headPillFg; // the HUN count pill
    };
    // A · ink — the 2026-08-18 ROM captures' own tones (opaque near-black;
    // ledger A-141): the island and its cards read as solid dark tiles, the
    // chevron/count discs as grey raised marks, no rims anywhere.
    inline constexpr SV13 INK = {
        .panel      = 0xFF0F1114, // opaque #0f1114 (the island)
        .card       = 0xFF1B1B1E, // opaque #1b1b1e
        .heads      = 0xFF1B1B1E, // the HUN wears the card fill
        .raised     = 0x1CFFFFFF, // white @11%
        .raisedH    = 0x2BFFFFFF, // white @17%
        .rowLine    = 0x1FFFFFF0, // white @12%
        .rim        = 0x00000000, // no borders on the shade surfaces
        .selRow     = 0x1AFFFFFF, // white @10%
        .selRowB    = 0x29FFFFFF, // white @16%
        .on         = 0xFFE8ECF2,
        .on82       = 0xD1E8ECF2,
        .on60       = 0x99E8ECF2,
        .on40       = 0x66E8ECF2,
        .action     = 0xFFA8C7FA, // ROM action text (distinct from the accent)
        .onAccent   = 0xFF06222E,
        .chip       = 0x33FFFFFF, // white @20% — the chevron/count disc
        .pillBg     = 0x33FFFFFF, // the count pill wears the same grey disc
        .pillFg     = 0xFFE8ECF2,
        .headPillBg = 0x33FFFFFF, // the HUN count pill, grey like the shade's
        .headPillFg = 0xFFE8ECF2,
    };
    // E · tray menu — the glass·ink set; its column matches the shared roles
    // above (PANEL/OUTLINE/STATE/PRIMARY/ON_PRIMARY/ON_SURFACE) by design.
    inline constexpr SV13 GLASS = {
        .panel      = 0x9E0F1218, // #0f1218 @62%
        .card       = 0x80090C12, // #090c12 @50%
        .heads      = 0xCC0F1218, // #0f1218 @80%
        .raised     = 0x0FFFFFFF, // white @6%
        .raisedH    = 0x1CFFFFFF, // white @11%
        .rowLine    = 0x1FDECBFF, // #dcebff @12%
        .rim        = 0x1FDECBFF,
        .selRow     = 0x2E32D6FF, // state cyan @18%
        .selRowB    = 0x00000000, // no outline
        .on         = 0xFFE4E8EE,
        .on82       = 0xD1E4E8EE,
        .on60       = 0x99E4E8EE,
        .on40       = 0x66E4E8EE,
        .action     = 0xFF32D6FF, // = PRIMARY
        .onAccent   = 0xFF07161C, // = ON_PRIMARY
        .chip       = 0x24DECBFF, // #dcebff @14%
        .pillBg     = 0x2E32D6FF, // state cyan @18%
        .pillFg     = 0xFF32D6FF,
        .headPillBg = 0x2E32D6FF,
        .headPillFg = 0xFF32D6FF,
    };
    // M · tray — the hyprbar tray icon's right-click menu material (the
    // shared PANEL/SURFACE/SURFACE_HIGH/STATE/PRIMARY/ON_SURFACE roles),
    // with the veils baked OPAQUE: the shade must read on any background
    // without a blur pass behind it, so no frost — user-directed default.
    // Over the opaque panel the translucent tokens render exactly their
    // bakes, so they stay material-true.
    inline constexpr SV13 TRAY = {
        .panel      = 0xFF0F1218, // opaque PANEL hue (#0f1218)
        .card       = 0xFF1D2026, // SURFACE white @6% baked on the panel
        .heads      = 0xFF0F1218, // the HUN veil wears the panel hue
        .raised     = 0x0FFFFFFF, // SURFACE white @6%
        .raisedH    = 0x1CFFFFFF, // SURFACE_HIGH white @11%
        .rowLine    = 0x1FDECBFF, // OUTLINE #dcebff @12%
        .rim        = 0x1FDECBFF, // the menu's frame ring
        .selRow     = 0x2E32D6FF, // STATE primary @18%
        .selRowB    = 0x00000000,
        .on         = 0xFFE4E8EE, // ON_SURFACE
        .on82       = 0xD1E4E8EE,
        .on60       = 0x99E4E8EE,
        .on40       = 0x66E4E8EE,
        .action     = 0xFF32D6FF, // = PRIMARY
        .onAccent   = 0xFF07161C, // = ON_PRIMARY
        .chip       = 0x1CFFFFFF, // SURFACE_HIGH @11% — the chevron disc
        .pillBg     = 0x2E32D6FF, // STATE primary @18%
        .pillFg     = 0xFF32D6FF,
        .headPillBg = 0x2E32D6FF,
        .headPillFg = 0xFF32D6FF,
    };

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
