// Every glyph's SVG is a document librsvg accepts with the viewBox it
// declares: a malformed shape (a repeated attribute, say) is refused whole
// and draws nothing, with no error at the call.
#include "../core/glyph_shapes.hpp"

#include "harness.hpp"

#include <librsvg/rsvg.h>

using namespace NAwesome::Glyph;

bool test_glyph() {
    bool ok = true;
    for (size_t i = 1; i < (size_t)eGlyph::COUNT_; i++) {
        const auto  SVG = svgSource((eGlyph)i);
        GError*     err = nullptr;
        RsvgHandle* H   = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(SVG.data()), SVG.size(), &err);
        if (!AW_CHECK(H != nullptr)) {
            std::printf("  glyph %zu: %s\n", i, err ? err->message : "refused");
            ok = false;
        }
        if (err)
            g_error_free(err);
        if (!H)
            continue;
        gboolean      hasW = false, hasH = false, hasBox = false;
        RsvgLength    w{}, h{};
        RsvgRectangle box{};
        rsvg_handle_get_intrinsic_dimensions(H, &hasW, &w, &hasH, &h, &hasBox, &box);
        ok &= AW_CHECK(hasBox && box.width == SHAPES[i].w && box.height == SHAPES[i].h);
        g_object_unref(H);
    }
    return ok;
}
