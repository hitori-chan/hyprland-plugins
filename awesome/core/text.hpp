// The one set of font options every plugin text raster is measured and
// drawn with.
#pragma once

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

namespace NAwesome {

    // Hinted, whole-pixel text: the compositor's renderText sets no options,
    // so Pango would hint only vertically and place glyphs at fractional
    // pixels (soft 1x text). A layout measured under other options than it
    // is drawn with wraps and ellipsizes at other widths: every context
    // comes from here.
    inline const cairo_font_options_t* textFontOptions() {
        static const struct SOptions {
            cairo_font_options_t* o = cairo_font_options_create();
            SOptions() {
                cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
                cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_FULL);
                cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_ON);
            }
            ~SOptions() {
                cairo_font_options_destroy(o);
            }
        } OPTIONS;
        return OPTIONS.o;
    }

    inline void applyTextOptions(PangoContext* ctx) {
        pango_cairo_context_set_font_options(ctx, textFontOptions());
        pango_context_set_round_glyph_positions(ctx, TRUE);
    }

    // A context on the shared font map, under the options above. The caller
    // owns the reference.
    inline PangoContext* newTextContext() {
        PangoContext* ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
        applyTextOptions(ctx);
        return ctx;
    }

    // A layout drawn into a cairo context it was not made from (fontwarm, a
    // badge letter): the same options.
    inline void applyTextOptions(PangoLayout* layout) {
        applyTextOptions(pango_layout_get_context(layout));
        pango_layout_context_changed(layout);
    }

    // Text over glass wears no shadow: the glass is dense enough to read on.
    // Text bare on the desktop (the center's heading, a stack's name)
    // keeps a desktop label's drop. The raster grows by pad() physical
    // px on every side: symmetric, so centering ignores it; every other
    // placement steps back by it.
    enum class eShadow : unsigned char {
        NONE,
        DESKTOP, // text bare on the wallpaper
    };
    struct STextShadow {
        int    r  = 0; // the 3-pass box blur's radius, physical px
        int    dy = 0; // the drop, physical px
        double a  = 0;
        int    pad() const {
            return r > 0 ? 3 * r + dy : 0;
        }
    };
    STextShadow textShadow(eShadow s, int pt);

    // Shows `layout` at (pad, pad) over its shadow, in rgba. The surface is
    // ink + 2 * pad on each axis.
    void showShadowedLayout(cairo_t* cr, PangoLayout* layout, int inkW, int inkH, const STextShadow& s, double r, double g, double b, double a);

    // the 3-pass box blur over an A8 coverage mask (in place)
    void blurA8(unsigned char* px, int w, int h, int stride, int r);

} // namespace NAwesome
