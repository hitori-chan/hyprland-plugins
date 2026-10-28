// awesome/core/tokens.hpp — the Frost material's tokens: the config
// defaults (core/schema.hpp's rows) and the fixed fills, radii and motion
// the painters use. Dependency-free, so the schema can name them.
#pragma once

#include <cstdint>

namespace NAwesome::Theme {

    // ---- the glass (0xAARRGGBB, config default form): one material for the
    //      bar, the cards, the center, the capsules, menus and the launcher ----
    // Mist: a 54 % grey over the compositor's blur, which darkens what it
    // samples (decoration:blur size 3, passes 2, brightness 0.48, contrast
    // 1, vibrancy 0.6 — the user's config). About a fifth of the backdrop
    // shows through, and the text needs no drop: titles 4.7:1 and
    // secondary 3.4:1 over a white page (measured on the mocks)
    inline constexpr uint32_t GLASS = 0x8a58585e; // 54 %

    // ---- ink: one white at four strengths, the blue as text only ----
    inline constexpr uint32_t TITLE     = 0xffededf0; // primary
    inline constexpr uint32_t BODY      = 0xdbebebf5; // body text, 86 %
    inline constexpr uint32_t SUB       = 0xbdebebf5; // secondary: meta, kickers, 74 %
    inline constexpr uint32_t DIM       = 0x57ebebf5; // tertiary: empty tags, minimized, 34 %
    inline constexpr uint32_t ACCENT    = 0xff00ccff; // selection and focus — text, never a ground (the bell's dot is the one mark)
    inline constexpr uint32_t ON_ACCENT = 0xff00141a; // dark ink on the accent: the live send arrow
    inline constexpr uint32_t LIT_INK   = 0xff0089ad; // a lit control's glyph on its white: the accent, deepened to read on it
    inline constexpr uint32_t URGENT    = 0xffff6159; // critical, urgent, low battery
    inline constexpr uint32_t LINK      = 0xff7db4ff; // body hyperlinks
    inline constexpr uint32_t CHARGING  = 0xff32d74b;
    inline constexpr uint32_t SAVER     = 0xffffd60a;
    inline constexpr uint32_t DOT       = 0xff00ccff; // the bell's unseen dot: the accent

    // ---- fills over glass ----
    inline constexpr uint32_t FILL        = 0x1fffffff; // buttons, chips, fields: white 12 %
    inline constexpr uint32_t FILL_HOVER  = 0x33ffffff; // the same under the pointer, 20 %
    inline constexpr uint32_t TRACK       = 0x29ffffff; // a progress or level track, 16 %
    inline constexpr uint32_t HAIRLINE    = 0x1affffff; // separators, 10 %
    inline constexpr uint32_t WELL        = 0x38000000; // a field sunk into the glass: the reply field, black 22 %
    inline constexpr uint32_t WELL_RIM    = 0x14ffffff; // its inner edge, 8 %
    inline constexpr uint32_t CONTROL     = 0x24ffffff; // a control on a card at rest: the send circle, 14 %
    inline constexpr uint32_t SEND_ARROW  = 0xb3ebebf5; // the send arrow at rest, 70 %
    inline constexpr uint32_t RIM         = 0x2effffff; // the glass edge, 18 %
    inline constexpr uint32_t LIGHT       = 0x3dffffff; // the top edge catching light, 24 %
    inline constexpr uint32_t SHADOW      = 0x47000000; // a card's drop, 28 %
    inline constexpr uint32_t MENU_SHADOW = 0x6b000000; // a menu floats higher, 42 %
    inline constexpr uint32_t BADGE_RIM   = 0xffffffff; // the identity badge's ring
    inline constexpr uint32_t BADGE_GROUND = 0xff2c2c32; // a symbolic icon's ink on the badge's white
    inline constexpr uint32_t NOTE        = 0xe0121216; // a name or a state under the pointer: a dark note, 88 %
    inline constexpr uint32_t NOTE_SHADOW = 0x4d000000; // its drop, 30 %

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
