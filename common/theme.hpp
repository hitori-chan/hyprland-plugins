// common/theme.hpp — Pixel/AOSP dark semantic defaults shared by the shell
// renderers. Plugins expose the roles they use as config values; this block is
// only their coherent startup palette and geometry, never hidden runtime
// state.
//
// Who actually sources it, so the next reader does not go looking for more:
// hyprnotify exposes panel, container, state, text, outline, primary, and
// error roles. hyprbar maps its existing configurable slots to the same roles;
// hyprsnap uses primary for its preview outline. Geometry follows the current
// circular Material family used by hyprnotify: 24px cards, 12px menu pills,
// rounding power 2.
#pragma once

#include <cstdint>

namespace NHyprCommon::Theme {

    // ---- Material roles -------------------------------------------------
    // 0xAARRGGBB — the format Config::Values::CColorValue defaults take.
    // The blue/teal values are measured from the supplied Pixel captures and
    // ROM-backed v6 reference. Dynamic color remains user configuration; a
    // fixed desktop default cannot claim wallpaper-derived Monet behavior.
    inline constexpr uint64_t PANEL               = 0xff132732; // shade/panel container; opaque skips blur by default
    inline constexpr uint64_t SURFACE             = 0xff172025; // notification/menu card
    inline constexpr uint64_t SURFACE_HIGH        = 0xff243944; // controls and raised state
    inline constexpr uint64_t STATE               = 0x339acbff; // primary @20% interaction layer
    inline constexpr uint64_t ON_SURFACE          = 0xffeef3f5; // body text and icons
    inline constexpr uint64_t ON_SURFACE_STRONG   = 0xfff3f6f7; // titles and emphasis
    inline constexpr uint64_t ON_SURFACE_VARIANT  = 0xffd1dde1; // metadata and secondary copy
    inline constexpr uint64_t ON_SURFACE_DISABLED = 0x61d1dde1; // disabled content @38%
    inline constexpr uint64_t PRIMARY             = 0xff9acbff; // actions, progress, focus
    inline constexpr uint64_t ON_PRIMARY          = 0xff102333; // content on a primary fill
    inline constexpr uint64_t ERROR               = 0xffffb4ab; // critical content
    inline constexpr uint64_t ON_ERROR            = 0xff690005; // content on an error fill
    inline constexpr uint64_t ERROR_CONTAINER     = 0xff93000a; // urgent selected container
    inline constexpr uint64_t OUTLINE             = 0x33e0f0f8; // hairlines @20%
    inline constexpr uint64_t SHADOW              = 0x73000000; // card shadow ink @45%
    inline constexpr uint64_t BADGE_RIM           = 0xfff5f7f8; // AOSP conversation badge background

    // ---- type -----------------------------------------------------------
    // Roboto is the AOSP UI family and is installed on the target. Pixel's
    // proprietary Google Sans is retained only in the self-contained design
    // study, where the font asset is explicit.
    inline constexpr const char* FONT = "Roboto";

    // ---- radii (logical px) ----------------------------------------------
    // hyprnotify derives panel/row/joint radii from its configured card value.
    // These constants serve hyprbar's fixed-height menus.
    inline constexpr int    RAD_CARD       = 24;
    inline constexpr int    RAD_ROW        = 12;
    inline constexpr double ROUNDING_POWER = 2.0;

    // ---- motion (ms; every plugin honors animations=0 as the kill switch) -
    inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival

} // namespace NHyprCommon::Theme
