// awesome/core/glyph.hpp — the shell's vector glyphs: the bell, the OSD's
// speaker/sun/mic/touchpad, the battery outline, chevrons, the moon. Small
// inline SVGs rastered by librsvg at the exact physical size, tinted, and
// cached for the session — the shell and the cards draw the same shapes
// from here instead of each carrying a path player.
//
// The texture rule holds (crash class 4): a miss builds only inside a warm
// (or a token); a draw-side miss returns null and flags the rewarm.
#pragma once

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <cstdint>

namespace NAwesome::Glyph {

    enum class eGlyph : uint8_t {
        NONE = 0,
        BELL,
        BELL_OFF,
        MOON,
        SPEAKER,
        SPEAKER_MUTED,
        SUN,
        MIC,
        MIC_OFF,
        TOUCHPAD,
        TOUCHPAD_OFF,
        BATTERY, // the outline only (22:11); the level is a rect the caller paints
        CHEVRON_UP,
        CHEVRON_DOWN,
        CHEVRON_RIGHT,
        CLOSE,
        SEND,
        CHECK,
        COUNT_,
    };

    // the glyph rastered at w x h physical px (h 0: square), tinted `col`
    SP<Render::ITexture> tex(eGlyph g, const CHyprColor& col, int w, int h = 0);
    // a glyph's own aspect, width over height (the battery is 2:1)
    double aspect(eGlyph g);
    void   clear(); // teardown: the textures go before the renderer does

} // namespace NAwesome::Glyph
