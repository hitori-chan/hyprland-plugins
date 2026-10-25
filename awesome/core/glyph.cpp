// awesome/core/glyph.cpp — see glyph.hpp. The shapes are the Frost boards'
// own (16-unit canvases, 1.4-unit strokes), drawn white and tinted after
// the raster so one source serves every colour.
#include "glyph.hpp"

#include "canvas.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <array>
#include <cmath>
#include <format>
#include <string_view>
#include <unordered_map>

namespace NAwesome::Glyph {

    namespace {
        struct SShape {
            double           w = 16, h = 16; // the viewBox
            std::string_view body;           // SVG elements, painted white
        };

#define AW_STROKE "fill=\"none\" stroke=\"#fff\" stroke-width=\"1.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\""
        constexpr std::array<SShape, (size_t)eGlyph::COUNT_> SHAPES = {{
            {}, // NONE
            {16, 16, "<path " AW_STROKE " d=\"M4 11V7.5a4 4 0 0 1 8 0V11l1.2 1.2H2.8zM6.6 13.8a1.5 1.5 0 0 0 2.8 0\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M4 11V7.5a4 4 0 0 1 6.2-3.3M12 7.5V11l1.2 1.2H5.5M6.6 13.8a1.5 1.5 0 0 0 2.8 0M2.5 2.5l11 11\"/>"},
            {16, 16, "<path fill=\"#fff\" d=\"M13.4 9.9A5.9 5.9 0 0 1 6.1 2.6a5.9 5.9 0 1 0 7.3 7.3z\"/>"},
            {16, 16, "<path fill=\"#fff\" d=\"M2.2 6.1h2.7l3.4-2.9v9.6l-3.4-2.9H2.2z\"/><path " AW_STROKE " d=\"M10.6 5.8a3.1 3.1 0 0 1 0 4.4M12.5 3.9a5.8 5.8 0 0 1 0 8.2\"/>"},
            {16, 16, "<path fill=\"#fff\" d=\"M2.2 6.1h2.7l3.4-2.9v9.6l-3.4-2.9H2.2z\"/><path " AW_STROKE " d=\"M10.8 6.2l3.6 3.6M14.4 6.2l-3.6 3.6\"/>"},
            {16, 16, "<circle fill=\"#fff\" cx=\"8\" cy=\"8\" r=\"2.9\"/><path " AW_STROKE
                     " d=\"M8 1.4v1.5M8 13.1v1.5M1.4 8h1.5M13.1 8h1.5M3.3 3.3l1.1 1.1M11.6 11.6l1.1 1.1M3.3 12.7l1.1-1.1M11.6 4.4l1.1-1.1\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M8 2a2.2 2.2 0 0 1 2.2 2.2v3.4a2.2 2.2 0 0 1-4.4 0V4.2A2.2 2.2 0 0 1 8 2zM4.2 7.6a3.8 3.8 0 0 0 7.6 0M8 11.4v2.4\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M10.2 6.4V4.2a2.2 2.2 0 0 0-4.2-.9M5.8 6.2v1.4a2.2 2.2 0 0 0 3.6 1.7M4.2 7.6a3.8 3.8 0 0 0 6.3 2.8M11.8 7.6l-.1.7M8 11.4v2.4M2.5 2.5l11 11\"/>"},
            {16, 16, "<rect " AW_STROKE " x=\"1.8\" y=\"2.8\" width=\"12.4\" height=\"10.4\" rx=\"2\"/><path " AW_STROKE " d=\"M1.8 9.6h12.4M8 9.6v3.6\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M4.6 2.8h7.6a2 2 0 0 1 2 2v6.5M13.4 13a2 2 0 0 1-1.2.2H3.8a2 2 0 0 1-2-2V4.8a2 2 0 0 1 .4-1.2M1.8 9.6h7.8M8 9.6v3.6M2.5 2.5l11 11\"/>"},
            {22, 11, "<rect fill=\"none\" stroke=\"#fff\" stroke-width=\"1.2\" x=\"0.7\" y=\"0.7\" width=\"18\" height=\"9.6\" rx=\"2.8\"/><path " AW_STROKE " d=\"M20.4 4v3\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M4 10l4-4 4 4\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M4 6l4 4 4-4\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M6 4l4 4-4 4\"/>"},
            {16, 16, "<path " AW_STROKE " d=\"M4.5 4.5l7 7M11.5 4.5l-7 7\"/>"},
            {16, 16, "<path " AW_STROKE " stroke-width=\"1.7\" d=\"M8 12.6V3.8M4.6 7.1L8 3.7l3.4 3.4\"/>"},
            {16, 16, "<path " AW_STROKE " stroke-width=\"1.6\" d=\"M3.4 8.4l3 3 6.2-6.6\"/>"},
        }};
#undef AW_STROKE

        // a handful of shapes x colours x sizes: past the bound the next
        // warm rebuilds what it still draws
        constexpr size_t MAX_ENTRIES = 256;

        std::unordered_map<uint64_t, SP<Render::ITexture>>& cache() {
            static std::unordered_map<uint64_t, SP<Render::ITexture>> C;
            return C;
        }

        SP<Render::ITexture> build(eGlyph g, const CHyprColor& col, int w, int h) {
            const auto& S   = SHAPES[(size_t)g];
            const auto  SVG = std::format(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {} {}">{}</svg>)", S.w, S.h, S.body);
            RsvgHandle* H   = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(SVG.data()), SVG.size(), nullptr);
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

    double aspect(eGlyph g) {
        const auto& S = SHAPES[(size_t)g];
        return S.w / S.h;
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
