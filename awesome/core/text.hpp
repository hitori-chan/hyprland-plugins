// awesome/core/text.hpp — the one set of font options every plugin text
// raster is measured and drawn with.
#pragma once

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

namespace NAwesome {

    // Hinted, whole-pixel text. The compositor's renderText sets no options,
    // so Pango hinted only vertically and placed glyphs at fractional pixels:
    // stems straddled two pixels and 1x text read soft beside a terminal's.
    // A layout measured under other options than it is drawn with wraps and
    // ellipsizes at other widths: every context comes from here.
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

} // namespace NAwesome
