// awesome/core/tokens.hpp — the Frost material's tokens: the config
// defaults (core/schema.hpp's rows) and the fixed fills, radii and motion
// the painters use. Dependency-free, so the schema can name them.
#pragma once

#include <cstdint>

namespace NAwesome::Theme {

    // ---- the three glass thicknesses (0xAARRGGBB, config default form) ----
    inline constexpr uint32_t BAR_GLASS  = 0x6b1c1c20; // thin: the bar
    inline constexpr uint32_t GLASS      = 0x8a26262c; // regular: cards, the center, capsules
    inline constexpr uint32_t MENU_GLASS = 0xad28282e; // thick: menus

    // ---- ink: one white at four strengths, the blue as text only ----
    inline constexpr uint32_t TITLE     = 0xffededf0; // primary
    inline constexpr uint32_t INK       = 0xdbebebf5; // the bar's text, 86 %
    inline constexpr uint32_t BODY      = 0xc7ebebf5; // body text, 78 %
    inline constexpr uint32_t SUB       = 0x8cebebf5; // secondary: meta, kickers, 55 %
    inline constexpr uint32_t DIM       = 0x57ebebf5; // tertiary: empty tags, minimized, 34 %
    inline constexpr uint32_t ACCENT    = 0xff00ccff; // selection and focus — text, never a fill
    inline constexpr uint32_t ON_ACCENT = 0xff00141a; // dark ink on the accent: the live send arrow
    inline constexpr uint32_t URGENT    = 0xffff6159; // critical, urgent, low battery
    inline constexpr uint32_t LINK      = 0xff7db4ff; // body hyperlinks
    inline constexpr uint32_t CHARGING  = 0xff32d74b;
    inline constexpr uint32_t SAVER     = 0xffffd60a;
    inline constexpr uint32_t DOT       = 0xff32d74b; // the bell's unseen dot (macOS green)

    // ---- fills over glass ----
    inline constexpr uint32_t FILL        = 0x1fffffff; // buttons, chips, fields: white 12 %
    inline constexpr uint32_t FILL_HOVER  = 0x33ffffff; // the same under the pointer, 20 %
    inline constexpr uint32_t TRACK       = 0x29ffffff; // a progress or level track, 16 %
    inline constexpr uint32_t HAIRLINE    = 0x1affffff; // separators, 10 %
    inline constexpr uint32_t WELL        = 0x38000000; // a field sunk into the glass: the reply field, black 22 %
    inline constexpr uint32_t WELL_RIM    = 0x14ffffff; // its inner edge, 8 %
    inline constexpr uint32_t CONTROL     = 0x24ffffff; // a control on a card at rest: Options, the send circle, 14 %
    inline constexpr uint32_t SEND_ARROW  = 0xb3ebebf5; // the send arrow at rest, 70 %
    inline constexpr uint32_t CHIP        = 0xb83a3a42; // a small control floating on glass: the close button, Show Less, 72 %
    inline constexpr uint32_t DECK_1      = 0x8a46464e; // a collapsed stack's first layer under its top card, 54 %
    inline constexpr uint32_t DECK_2      = 0x753e3e46; // its second, dimmer and darker, 46 %
    inline constexpr uint32_t RIM         = 0x1affffff; // the glass edge, 10 %
    inline constexpr uint32_t LIGHT       = 0x1affffff; // the top edge catching light, 10 %
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
    // the bar, its menus and the launcher: the terminals' face; Fira Code has
    // no CJK, and fontconfig's fallback for the kanji tags is a bitmap
    // Chinese face — IBM Plex Sans JP is drawn for them
    inline constexpr const char* BAR_FONT = "Fira Code, IBM Plex Sans JP";

    // ---- motion (ms) ----
    inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival

} // namespace NAwesome::Theme
