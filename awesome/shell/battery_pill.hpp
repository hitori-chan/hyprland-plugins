// awesome/shell/battery_pill.hpp — the bar's battery glyph, after macOS 27's
// menu-bar battery: a stadium body whose fill is the level, the number cut
// out of body and fill alike in the bar's own face, a short cap one unit off
// the body, and after the cap the attribution glyph (the charging bolt, the
// charge-limit shield, the unknown-state question). Pure cairo + Pango, so
// the unit harness rasters it with no compositor.
#pragma once

#include "shell/battery_state.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace NAwesome::Shell::Battery {

    // Measured off macOS 27's 2x menu bar, in points of a 12-unit-tall body:
    // a 21 x 12 stadium, the cap 1.5 x 4 one unit off it, the attribution
    // glyph 9 units tall two units past the cap. Every part rounds to whole
    // device pixels on its own, so each edge lands on the pixel grid.
    inline constexpr double VIEW_H = 12, BODY_W = 21, CAP_GAP = 1, CAP_W = 1.5, CAP_H = 4, GLYPH_GAP = 2, GLYPH_H = 9;
    // the unfilled body: white over the bar's glass, as dim as macOS draws it
    inline constexpr double TRACK_ALPHA = 0.40;
    // the number's pixel size per body pixel: digits ~7/12 of the body tall
    inline constexpr double DIGIT_SCALE = 0.84;

    constexpr int pixelRound(double v) {
        return static_cast<int>(v + 0.5);
    }

    // the attribution glyph's width over its height; 0 = none drawn. Low
    // Power Mode is its yellow fill alone, as on macOS.
    constexpr double attributionAspect(eAttribution a) {
        switch (a) {
            case eAttribution::Charging:
            case eAttribution::Defender: return 8.0 / 9.0;
            case eAttribution::Unknown: return 6.0 / 10.0;
            case eAttribution::PowerSave:
            case eAttribution::None: return 0.0;
        }
        return 0.0;
    }

    struct SGeometry {
        int bodyW = 0, bodyH = 0, capGap = 0, capW = 0, capH = 0, glyphGap = 0, glyphW = 0, glyphH = 0, width = 0;
    };

    // heightPx: the body's height in device px (an even one centres on an
    // even bar without a half-pixel seam)
    constexpr SGeometry geometry(int heightPx, eAttribution a) {
        const double S = heightPx / VIEW_H;
        SGeometry    g;
        g.bodyH  = heightPx;
        g.bodyW  = pixelRound(BODY_W * S);
        g.capGap = std::max(1, pixelRound(CAP_GAP * S));
        g.capW   = std::max(1, pixelRound(CAP_W * S));
        g.capH   = pixelRound(CAP_H * S);
        g.capH += (g.bodyH - g.capH) % 2; // centred on whole pixels
        g.width = g.bodyW + g.capGap + g.capW;
        if (const double AR = attributionAspect(a); AR > 0) {
            g.glyphGap = pixelRound(GLYPH_GAP * S);
            g.glyphH   = pixelRound(GLYPH_H * S);
            g.glyphH += (g.bodyH - g.glyphH) % 2;
            g.glyphW = std::max(1, pixelRound(g.glyphH * AR));
            g.width += g.glyphGap + g.glyphW;
        }
        return g;
    }

    // the fill's width in px for a level: proportional, and never a level
    // above zero that draws nothing
    constexpr int fillWidth(int level, int bodyW) {
        if (level <= 0)
            return 0;
        return std::clamp(pixelRound(level * bodyW / 100.0), 1, bodyW);
    }

    namespace Detail {
        // BatteryGlyph's attribution shapes (Pixel SystemUI, Apache-2.0),
        // verbatim: the bolt and the shield on 8 x 9, the question on 6 x 10
        inline constexpr const char* BOLT =
            "M7.672,3.375L4.302,3.375L5.038,0.563C5.113,0.281 4.91,0 4.633,0C4.515,0 4.398,0.056 4.324,0.146L0.09,5.051C-0.102,5.276 0.047,5.625 "
            "0.335,5.625L3.705,5.625L2.969,8.438C2.895,8.719 3.097,9 3.374,9C3.492,9 3.609,8.944 3.684,8.854L7.917,3.949C8.109,3.724 7.96,3.375 7.672,3.375Z";
        inline constexpr const char* DEFEND =
            "M-0.004,2.337C-0.004,2.09 0.145,1.865 0.373,1.787C1.048,1.551 2.487,0.989 3.642,0.124C3.745,0.045 3.87,0 3.996,0C4.122,0 4.247,0.045 "
            "4.35,0.124C5.504,0.989 6.944,1.551 7.619,1.787C7.859,1.865 8.007,2.09 7.996,2.337C7.801,7.461 4.236,9 3.996,9C3.756,9 0.191,7.461 "
            "-0.004,2.337ZM3.996,7.449C4.259,7.285 4.605,7.024 4.956,6.643C5.616,5.927 6.335,4.749 6.526,2.885C5.841,2.614 4.892,2.187 3.996,1.602L3.996,7.449Z";
        inline constexpr const char* QUESTION =
            "M2.85,6.438C2.591,6.438 2.363,6.356 2.167,6.193C1.975,6.025 1.879,5.823 1.879,5.588V5.557C1.879,5.209 1.958,4.911 2.117,4.663C2.276,4.416 2.545,4.143 "
            "2.925,3.845C3.276,3.572 3.537,3.346 3.708,3.166C3.883,2.985 3.971,2.792 3.971,2.587C3.971,2.31 3.869,2.091 3.664,1.932C3.464,1.768 3.188,1.687 "
            "2.837,1.687C2.616,1.687 2.418,1.722 2.242,1.794C2.067,1.865 1.919,1.961 1.798,2.083C1.677,2.205 1.568,2.322 1.472,2.435C1.38,2.545 1.242,2.62 "
            "1.059,2.662C0.879,2.7 0.687,2.67 0.482,2.574C0.282,2.477 0.14,2.32 0.057,2.102C-0.023,1.884 -0.019,1.668 0.069,1.454C0.161,1.24 0.34,1.015 "
            "0.608,0.78C0.879,0.541 1.207,0.352 1.591,0.214C1.975,0.071 2.426,0 2.944,0C3.866,0 4.605,0.231 5.161,0.692C5.72,1.15 6,1.739 6,2.461C6,2.897 "
            "5.889,3.287 5.668,3.631C5.451,3.971 5.098,4.315 4.61,4.663C4.326,4.869 4.127,5.049 4.015,5.205C3.902,5.36 3.835,5.546 3.814,5.765V5.777C3.781,5.945 "
            "3.679,6.098 3.507,6.237C3.336,6.371 3.117,6.438 2.85,6.438ZM2.837,10C2.495,10 2.205,9.885 1.967,9.654C1.733,9.423 1.616,9.14 1.616,8.804C1.616,8.477 "
            "1.733,8.2 1.967,7.974C2.205,7.747 2.495,7.634 2.837,7.634C3.18,7.634 3.47,7.747 3.708,7.974C3.95,8.2 4.071,8.477 4.071,8.804C4.071,9.14 3.952,9.423 "
            "3.714,9.654C3.476,9.885 3.184,10 2.837,10Z";

        // just enough SVG for the paths above: absolute M/L/H/V/C/Z
        inline void playPath(cairo_t* cr, const char* d) {
            const char* p   = d;
            char        cmd = 0;
            double      x = 0, y = 0;
            const auto  num = [&]() {
                while (*p == ',' || *p == ' ')
                    p++;
                char*        e;
                const double V = std::strtod(p, &e);
                p              = e;
                return V;
            };
            while (*p) {
                if (*p == ',' || *p == ' ') {
                    p++;
                    continue;
                }
                if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                    cmd = *p++;
                    if (cmd == 'Z') {
                        cairo_close_path(cr);
                        cmd = 0;
                        continue;
                    }
                }
                switch (cmd) {
                    case 'M':
                        x = num(), y = num();
                        cairo_move_to(cr, x, y);
                        cmd = 'L'; // SVG: further pairs after M are lineto
                        break;
                    case 'L':
                        x = num(), y = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'H':
                        x = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'V':
                        y = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'C': {
                        const double C1X = num(), C1Y = num(), C2X = num(), C2Y = num();
                        x = num(), y = num();
                        cairo_curve_to(cr, C1X, C1Y, C2X, C2Y, x, y);
                        break;
                    }
                    default: return; // unknown command: stop rather than misdraw
                }
            }
        }

        struct SPathGlyph {
            double      w, h;
            const char* d;
        };
        inline const SPathGlyph* attributionGlyph(eAttribution a) {
            static const SPathGlyph BOLT_G{8, 9, BOLT}, DEFEND_G{8, 9, DEFEND}, QUESTION_G{6, 10, QUESTION};
            switch (a) {
                case eAttribution::Charging: return &BOLT_G;
                case eAttribution::Defender: return &DEFEND_G;
                case eAttribution::Unknown: return &QUESTION_G;
                case eAttribution::PowerSave:
                case eAttribution::None: return nullptr;
            }
            return nullptr;
        }
    } // namespace Detail

    struct SRgba {
        double r = 1, g = 1, b = 1, a = 1;
    };

    struct SPillSpec {
        int          level = -1; // -1: unknown, no number
        eAttribution attribution = eAttribution::None;
        int          heightPx    = 12;
        SRgba        fill;  // the level: white, or the state's colour
        SRgba        glyph; // the attribution glyph: the bar's text colour
        std::string  family; // the bar's face (a Pango family list)
    };

    // The pill on a fresh ARGB32 surface of geometry().width x heightPx; the
    // caller owns it. Null if cairo cannot allocate.
    inline cairo_surface_t* rasterPill(const SPillSpec& spec) {
        const auto G    = geometry(spec.heightPx, spec.attribution);
        auto*      SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, G.width, G.bodyH);
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        auto*        CR   = cairo_create(SURF);
        const bool   HAS  = spec.level >= 0;
        const int    LVL  = std::clamp(spec.level, 0, 100);
        const bool   FULL = HAS && LVL >= 100;
        const double R    = G.bodyH / 2.0;

        const auto body = [&]() {
            cairo_new_sub_path(CR);
            cairo_arc(CR, R, R, R, M_PI / 2, 3 * M_PI / 2);
            cairo_arc(CR, G.bodyW - R, R, R, -M_PI / 2, M_PI / 2);
            cairo_close_path(CR);
        };
        const auto source = [&](const SRgba& c) { cairo_set_source_rgba(CR, c.r, c.g, c.b, c.a); };
        const SRgba TRACK{1, 1, 1, TRACK_ALPHA};

        // the body: the track, and the level over it (all of it when full)
        source(FULL ? spec.fill : TRACK);
        body();
        cairo_fill(CR);
        if (const int FW = fillWidth(LVL, G.bodyW); HAS && !FULL && FW > 0) {
            cairo_save(CR);
            cairo_rectangle(CR, 0, 0, FW, G.bodyH);
            cairo_clip(CR);
            source(spec.fill);
            body();
            cairo_fill(CR);
            cairo_restore(CR);
        }

        // the number, cut out of body and fill alike: the glass shows through
        if (HAS) {
            PangoLayout* L = pango_cairo_create_layout(CR);
            applyTextOptions(L);
            PangoFontDescription* FD = pango_font_description_from_string(spec.family.c_str());
            pango_font_description_set_weight(FD, PANGO_WEIGHT_BOLD);
            pango_font_description_set_absolute_size(FD, (double)pixelRound(G.bodyH * DIGIT_SCALE) * PANGO_SCALE);
            pango_layout_set_font_description(L, FD);
            pango_font_description_free(FD);
            // the plain zero: a mono face's dotted one is a blot this small
            PangoAttrList* AL = pango_attr_list_new();
            pango_attr_list_insert(AL, pango_attr_font_features_new("cv11 0, zero 0"));
            pango_layout_set_attributes(L, AL);
            pango_attr_list_unref(AL);
            const std::string TXT = std::to_string(LVL);
            pango_layout_set_text(L, TXT.c_str(), -1);
            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(L, &ink, &logical);
            // the ink, not the line box, centred on whole pixels in the body
            const int X = (int)std::lround((G.bodyW - ink.width) / 2.0) - ink.x;
            const int Y = (int)std::lround((G.bodyH - ink.height) / 2.0) - ink.y;
            cairo_move_to(CR, X, Y);
            pango_cairo_layout_path(CR, L);
            cairo_set_operator(CR, CAIRO_OPERATOR_CLEAR);
            cairo_fill(CR);
            cairo_set_operator(CR, CAIRO_OPERATOR_OVER);
            g_object_unref(L);
        }

        // the cap: rounded on its outer side only
        {
            const double X = G.bodyW + G.capGap, Y = (G.bodyH - G.capH) / 2.0, W = G.capW, H = G.capH;
            const double CR_ = std::min(W, H / 2.0);
            cairo_new_sub_path(CR);
            cairo_move_to(CR, X, Y);
            cairo_arc(CR, X + W - CR_, Y + CR_, CR_, -M_PI / 2, 0);
            cairo_arc(CR, X + W - CR_, Y + H - CR_, CR_, 0, M_PI / 2);
            cairo_line_to(CR, X, Y + H);
            cairo_close_path(CR);
            source(TRACK);
            cairo_fill(CR);
        }

        if (const auto* GL = Detail::attributionGlyph(spec.attribution); GL && G.glyphW > 0) {
            const double X = G.bodyW + G.capGap + G.capW + G.glyphGap, Y = (G.bodyH - G.glyphH) / 2.0;
            const double S = std::min(G.glyphW / GL->w, G.glyphH / GL->h);
            cairo_save(CR);
            cairo_translate(CR, X + (G.glyphW - GL->w * S) / 2.0, Y + (G.glyphH - GL->h * S) / 2.0);
            cairo_scale(CR, S, S);
            Detail::playPath(CR, GL->d);
            cairo_restore(CR);
            source(spec.glyph);
            cairo_fill(CR);
        }

        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        return SURF;
    }

} // namespace NAwesome::Shell::Battery
