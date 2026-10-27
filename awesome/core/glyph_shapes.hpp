// awesome/core/glyph_shapes.hpp — the glyphs' SVG sources, kept free of
// compositor headers so the unit harness parses every one: librsvg refuses
// a malformed shape whole and the glyph then silently never draws.
#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace NAwesome::Glyph {

    enum class eGlyph : uint8_t {
        NONE = 0,
        BELL,
        BELL_BADGE, // the bell cut free of the dot that rides its shoulder
        MOON,
        MOON_BADGE,
        SPEAKER,
        SPEAKER_MUTED,
        SUN,
        MIC,
        MIC_OFF,
        TOUCHPAD,
        TOUCHPAD_OFF,
        BADGE_DOT, // the dot alone, drawn over a *_BADGE in its own colour
        BATTERY,   // the plug capsule's outline (22:11); the bar's battery is shell/battery_pill.hpp
        CHEVRON_UP,
        CHEVRON_DOWN,
        CHEVRON_RIGHT,
        CLOSE,
        SEND,
        CHECK,
        DISMISS, // a stack header's close cross, drawn at 8 px on its own 10-unit grid
        KEYBOARD_LIGHT, // a half sun risen over a baseline: the keys' own light
        CONTROLS,       // the control center's door on the bar: two switches, SF Symbols' switch.2
        SUN_LOW,        // a slider's low end: the sun, smaller, its rays short
        KEYBOARD,       // the keyboard light's low end: the keys, unlit
        KEYBOARD_BRIGHT, // its high end: the keys under rays
        SPEAKER_LOW,    // the volume's low end: the speaker without its waves
        ROOM,           // follow the room: an A in the sun's rays
        OUTPUT,         // the volume's output: a loudspeaker
        COUNT_,
    };

    struct SShape {
        double           w = 16, h = 16; // the viewBox
        std::string_view body;           // SVG elements, painted white
    };

// one stroke-width per element: a repeated attribute is malformed XML,
// librsvg refuses the whole shape and the glyph never draws
#define AW_STROKE_W(W) "fill=\"none\" stroke=\"#fff\" stroke-width=\"" W "\" stroke-linecap=\"round\" stroke-linejoin=\"round\""
#define AW_STROKE AW_STROKE_W("1.4")
// the bell drawn on the 16 px grid it rasters at on a 1x bar: its stems and
// skirt sit on half-pixel centres, so each lands on whole pixels
#define AW_BELL "<path " AW_STROKE " d=\"M4.5 11V7.5a3.5 3.5 0 0 1 7 0V11l1.5 1.5H3z\"/><path fill=\"#fff\" d=\"M6.8 13.9h2.4a1.2 1.2 0 0 1-2.4 0z\"/>"
// SF Symbols' bell.badge: the dot on the upper-right shoulder, the shape
// cut back around it so the dot floats free of the stroke
#define AW_BADGE_X "12.5"
#define AW_BADGE_Y "3.5"
#define AW_BADGE_CUT "<circle fill=\"#000\" cx=\"" AW_BADGE_X "\" cy=\"" AW_BADGE_Y "\" r=\"3.6\"/>"
    inline constexpr std::array<SShape, (size_t)eGlyph::COUNT_> SHAPES = {{
        {}, // NONE
        {16, 16, AW_BELL},
        {16, 16, "<mask id=\"m\"><rect width=\"16\" height=\"16\" fill=\"#fff\"/>" AW_BADGE_CUT "</mask><g mask=\"url(#m)\">" AW_BELL "</g>"},
        {16, 16, "<path fill=\"#fff\" d=\"M13.4 9.9A5.9 5.9 0 0 1 6.1 2.6a5.9 5.9 0 1 0 7.3 7.3z\"/>"},
        {16, 16, "<mask id=\"m\"><rect width=\"16\" height=\"16\" fill=\"#fff\"/>" AW_BADGE_CUT "</mask><path mask=\"url(#m)\" fill=\"#fff\""
                 " d=\"M13.4 9.9A5.9 5.9 0 0 1 6.1 2.6a5.9 5.9 0 1 0 7.3 7.3z\"/>"},
        {16, 16, "<path fill=\"#fff\" d=\"M2.2 6.1h2.7l3.4-2.9v9.6l-3.4-2.9H2.2z\"/><path " AW_STROKE " d=\"M10.6 5.8a3.1 3.1 0 0 1 0 4.4M12.5 3.9a5.8 5.8 0 0 1 0 8.2\"/>"},
        {16, 16, "<path fill=\"#fff\" d=\"M2.2 6.1h2.7l3.4-2.9v9.6l-3.4-2.9H2.2z\"/><path " AW_STROKE " d=\"M10.8 6.2l3.6 3.6M14.4 6.2l-3.6 3.6\"/>"},
        {16, 16, "<circle fill=\"#fff\" cx=\"8\" cy=\"8\" r=\"2.9\"/><path " AW_STROKE
                 " d=\"M8 1.4v1.5M8 13.1v1.5M1.4 8h1.5M13.1 8h1.5M3.3 3.3l1.1 1.1M11.6 11.6l1.1 1.1M3.3 12.7l1.1-1.1M11.6 4.4l1.1-1.1\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M8 2a2.2 2.2 0 0 1 2.2 2.2v3.4a2.2 2.2 0 0 1-4.4 0V4.2A2.2 2.2 0 0 1 8 2zM4.2 7.6a3.8 3.8 0 0 0 7.6 0M8 11.4v2.4\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M10.2 6.4V4.2a2.2 2.2 0 0 0-4.2-.9M5.8 6.2v1.4a2.2 2.2 0 0 0 3.6 1.7M4.2 7.6a3.8 3.8 0 0 0 6.3 2.8M11.8 7.6l-.1.7M8 11.4v2.4M2.5 2.5l11 11\"/>"},
        {16, 16, "<rect " AW_STROKE " x=\"1.8\" y=\"2.8\" width=\"12.4\" height=\"10.4\" rx=\"2\"/><path " AW_STROKE " d=\"M1.8 9.6h12.4M8 9.6v3.6\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M4.6 2.8h7.6a2 2 0 0 1 2 2v6.5M13.4 13a2 2 0 0 1-1.2.2H3.8a2 2 0 0 1-2-2V4.8a2 2 0 0 1 .4-1.2M1.8 9.6h7.8M8 9.6v3.6M2.5 2.5l11 11\"/>"},
        {16, 16, "<circle fill=\"#fff\" cx=\"" AW_BADGE_X "\" cy=\"" AW_BADGE_Y "\" r=\"2.5\"/>"},
        {22, 11, "<rect " AW_STROKE_W("1.2") " x=\"0.7\" y=\"0.7\" width=\"18\" height=\"9.6\" rx=\"2.8\"/><path " AW_STROKE " d=\"M20.4 4v3\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M4 10l4-4 4 4\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M4 6l4 4 4-4\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M6 4l4 4-4 4\"/>"},
        {16, 16, "<path " AW_STROKE " d=\"M4.5 4.5l7 7M11.5 4.5l-7 7\"/>"},
        {16, 16, "<path " AW_STROKE_W("1.7") " d=\"M8 12.6V3.8M4.6 7.1L8 3.7l3.4 3.4\"/>"},
        {16, 16, "<path " AW_STROKE_W("1.6") " d=\"M3.4 8.4l3 3 6.2-6.6\"/>"},
        {10, 10, "<path " AW_STROKE_W("1.6") " d=\"M2 2l6 6M8 2l-6 6\"/>"},
        {16, 16, "<path fill=\"#fff\" d=\"M4.4 11.6a3.6 3.6 0 0 1 7.2 0z\"/><path " AW_STROKE " d=\"M1.6 11.6h12.8M8 6.4V4.6M4.32 7.92l-1.27-1.27M11.68 7.92l1.27-1.27\"/>"},
        {16, 16, "<mask id=\"m\"><rect width=\"16\" height=\"16\" fill=\"#fff\"/><circle fill=\"#000\" cx=\"11.6\" cy=\"11.9\" r=\"1.9\"/></mask><rect " AW_STROKE_W("1.3")
                 " x=\"1.65\" y=\"1.65\" width=\"12.7\" height=\"5.4\" rx=\"2.7\"/><circle fill=\"#fff\" cx=\"4.4\" cy=\"4.35\" r=\"1.45\"/><rect fill=\"#fff\" mask=\"url(#m)\""
                 " x=\"1\" y=\"8.7\" width=\"14\" height=\"6.4\" rx=\"3.2\"/>"},
        {16, 16, "<circle fill=\"#fff\" cx=\"8\" cy=\"8\" r=\"2.4\"/><path " AW_STROKE
                 " d=\"M8 2.4v.8M8 12.8v.8M2.4 8h.8M12.8 8h.8M4 4l.6.6M11.4 11.4l.6.6M4 12l.6-.6M11.4 4.6l.6-.6\"/>"},
        {16, 16, "<rect " AW_STROKE " x=\"1.5\" y=\"6\" width=\"13\" height=\"8\" rx=\"1.8\"/><path " AW_STROKE " d=\"M4.5 9h.01M8 9h.01M11.5 9h.01M5.5 11.5h5\"/>"},
        {16, 16, "<rect " AW_STROKE " x=\"1.5\" y=\"6\" width=\"13\" height=\"8\" rx=\"1.8\"/><path " AW_STROKE
                 " d=\"M4.5 9h.01M8 9h.01M11.5 9h.01M5.5 11.5h5M8 1.2v2.2M3.6 2.5l1.1 1.7M12.4 2.5l-1.1 1.7\"/>"},
        {16, 16, "<path fill=\"#fff\" d=\"M2.2 6.1h2.7l3.4-2.9v9.6l-3.4-2.9H2.2z\"/>"},
        {16, 16, "<path " AW_STROKE_W("1.3") " d=\"M8 .9v1.5M8 13.6v1.5M.9 8h1.5M13.6 8h1.5M3 3l1 1M12 12l1 1M3 13l1-1M12 4l1-1\"/><path " AW_STROKE_W("1.3")
                 " d=\"M5.7 11.2L8 4.9l2.3 6.3M6.5 9.3h3\"/>"},
        {16, 16, "<rect " AW_STROKE " x=\"4\" y=\"1.5\" width=\"8\" height=\"13\" rx=\"2\"/><circle " AW_STROKE " cx=\"8\" cy=\"10\" r=\"2\"/><circle fill=\"#fff\" cx=\"8\" cy=\"4.8\" r=\".8\"/>"},
    }};
#undef AW_STROKE
#undef AW_STROKE_W
#undef AW_BADGE_X
#undef AW_BADGE_Y
#undef AW_BADGE_CUT
#undef AW_BELL

    // the shape as a whole document, drawn white
    inline std::string svgSource(eGlyph g) {
        const auto& S = SHAPES[(size_t)g];
        return std::format(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {} {}">{}</svg>)", S.w, S.h, S.body);
    }

} // namespace NAwesome::Glyph
