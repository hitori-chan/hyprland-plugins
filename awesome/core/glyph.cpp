// awesome/core/glyph.cpp — see glyph.hpp. The shapes are the Frost boards'
// own (16-unit canvases, 1.4-unit strokes), drawn white and tinted after
// the raster so one source serves every colour.
#include "glyph.hpp"

#include "canvas.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <cmath>
#include <unordered_map>

namespace NAwesome::Glyph {

    namespace {
        // a handful of shapes x colours x sizes: past the bound the next
        // warm rebuilds what it still draws
        constexpr size_t MAX_ENTRIES = 256;

        std::unordered_map<uint64_t, SP<Render::ITexture>>& cache() {
            static std::unordered_map<uint64_t, SP<Render::ITexture>> C;
            return C;
        }

        SP<Render::ITexture> build(eGlyph g, const CHyprColor& col, int w, int h) {
            const auto SVG = svgSource(g);
            RsvgHandle* H = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(SVG.data()), SVG.size(), nullptr);
            if (!H)
                return nullptr;
            auto*         SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            auto*         CR   = cairo_create(SURF);
            RsvgRectangle viewport{0, 0, (double)w, (double)h};
            rsvg_handle_render_document(H, CR, &viewport, nullptr);
            cairo_destroy(CR);
            g_object_unref(H);
            cairo_surface_flush(SURF);

            // white coverage -> the colour, premultiplied (cairo's ARGB32 is
            // B,G,R,A in memory on little-endian)
            auto*        D      = cairo_image_surface_get_data(SURF);
            const int    STRIDE = cairo_image_surface_get_stride(SURF);
            const double R = col.r * col.a, G = col.g * col.a, B = col.b * col.a;
            for (int y = 0; y < h; y++) {
                auto* row = D + (size_t)y * STRIDE;
                for (int x = 0; x < w; x++) {
                    const double A = row[x * 4 + 3] / 255.0;
                    row[x * 4]     = (uint8_t)std::lround(B * A * 255);
                    row[x * 4 + 1] = (uint8_t)std::lround(G * A * 255);
                    row[x * 4 + 2] = (uint8_t)std::lround(R * A * 255);
                    row[x * 4 + 3] = (uint8_t)std::lround(col.a * A * 255);
                }
            }
            cairo_surface_mark_dirty(SURF);
            auto tex = g_pHyprRenderer->createTexture(SURF);
            cairo_surface_destroy(SURF);
            return tex;
        }
    }

    SP<Render::ITexture> tex(eGlyph g, const CHyprColor& col, int w, int h) {
        if (g == eGlyph::NONE || g >= eGlyph::COUNT_)
            return nullptr;
        if (h <= 0)
            h = w;
        if (w < 2 || h < 2 || w > 512 || h > 512)
            return nullptr;
        const uint64_t KEY = (uint64_t)col.getAsHex() << 32 | (uint64_t)g << 24 | (uint64_t)w << 12 | (uint64_t)h; // w, h < 4096
        auto&          C   = cache();
        if (const auto IT = C.find(KEY); IT != C.end())
            return IT->second;
        if (!Canvas::inst().gate().mayBuild())
            return nullptr;
        if (C.size() >= MAX_ENTRIES)
            C.clear();
        return C.emplace(KEY, build(g, col, w, h)).first->second;
    }

    void clear() {
        cache().clear();
    }

} // namespace NAwesome::Glyph
