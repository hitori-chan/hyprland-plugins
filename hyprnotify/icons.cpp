// hyprnotify/icons.cpp — notification images: files via hyprgraphics, raw image-data

#include "hyprnotify.hpp"

#include <cstring>

namespace NHyprnotify {

    // Anything bigger than the card's icon box is downscaled ONCE on the CPU
    // at load — a 4K pixmap kept full-size would hold megabytes of VRAM to
    // paint <=100 logical px, and its GL upload would stall the main thread.
    static SP<ITexture> scaledTex(cairo_surface_t* src, double sw, double sh, int maxPx) {
        if (sw <= maxPx && sh <= maxPx)
            return g_pHyprRenderer->createTexture(src);

        const double SCALE = std::min(maxPx / sw, maxPx / sh);
        const int    W     = std::max(1, (int)std::lround(sw * SCALE));
        const int    H     = std::max(1, (int)std::lround(sh * SCALE));

        auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR    = cairo_create(SMALL);
        cairo_scale(CR, W / sw, H / sh);
        cairo_set_source_surface(CR, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(SMALL);

        auto tex = g_pHyprRenderer->createTexture(SMALL);
        cairo_surface_destroy(SMALL);
        return tex;
    }

    // CImage's size hint only bounds SVG rasters; raster formats decode full
    // size transiently and get scaled here.
    static SP<ITexture> fileTex(const std::string& path, int maxPx) {
        Hyprgraphics::CImage image(path, Vector2D{(double)maxPx, (double)maxPx});
        if (!image.success())
            return nullptr;

        const auto SURF = image.cairoSurface();
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return nullptr;

        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0)
            return nullptr;
        return scaledTex(SURF->cairo(), SZ.x, SZ.y, maxPx);
    }

    // CPU-side cap for image-data buffers at unpack time (bus.cpp) — same
    // premultiplied-BGRA layout in and out, so warm's hash/upload path is
    // untouched. Row-copied out: cairo's stride is its own business.
    void shrinkPixels(SNotif& n, int maxPx) {
        if (n.pixels.empty() || (n.pw <= maxPx && n.ph <= maxPx))
            return;

        auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
        if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SRC);
            return;
        }

        const double SCALE = std::min((double)maxPx / n.pw, (double)maxPx / n.ph);
        const int    W     = std::max(1, (int)std::lround(n.pw * SCALE));
        const int    H     = std::max(1, (int)std::lround(n.ph * SCALE));

        auto*        DST = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR  = cairo_create(DST);
        cairo_scale(CR, (double)W / n.pw, (double)H / n.ph);
        cairo_set_source_surface(CR, SRC, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(DST);
        cairo_surface_destroy(SRC);

        if (cairo_surface_status(DST) == CAIRO_STATUS_SUCCESS) {
            const int   STRIDE = cairo_image_surface_get_stride(DST);
            const auto* DATA   = cairo_image_surface_get_data(DST);
            std::vector<uint8_t> out((size_t)W * H * 4);
            for (int y = 0; y < H; y++)
                std::memcpy(out.data() + (size_t)y * W * 4, DATA + (size_t)y * STRIDE, (size_t)W * 4);
            n.pixels = std::move(out);
            n.pw     = W;
            n.ph     = H;
        }
        cairo_surface_destroy(DST);
    }

    static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
        const auto* P = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++)
            h = (h ^ P[i]) * 0x100000001b3ULL;
        return h;
    }

    void ensureIconTex(SNotif& n, int maxPx) {
        if (n.hasPixels) {
            if (n.pixels.empty())
                return; // uploaded by an earlier warm; the texture carries it now

            uint64_t h = fnv1a(n.pixels.data(), n.pixels.size(), 0xcbf29ce484222325ULL);
            h          = fnv1a(&n.pw, sizeof(n.pw), h);
            h          = fnv1a(&n.ph, sizeof(n.ph), h);
            if (!n.iconTex || n.pixelsFor != h) {
                if (n.pw > maxPx || n.ph > maxPx) {
                    // stride pw*4 is how unpackImageData lays the buffer out
                    auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
                    n.iconTex = cairo_surface_status(SRC) == CAIRO_STATUS_SUCCESS ? scaledTex(SRC, n.pw, n.ph, maxPx) : nullptr;
                    cairo_surface_destroy(SRC);
                } else
                    n.iconTex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, n.pixels.data(), n.pw * 4, Vector2D{(double)n.pw, (double)n.ph});
                n.pixelsFor = h;
                n.imageFor.clear();
            }
            n.pixels.clear();
            n.pixels.shrink_to_fit();
            return;
        }

        if (n.image.empty()) {
            n.iconTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            return;
        }
        if (n.imageFor == n.image) // also remembers a failed load: no disk retry per warm
            return;
        n.iconTex   = fileTex(n.image, maxPx);
        n.imageFor  = n.image;
        n.pixelsFor = 0;
    }

} // namespace NHyprnotify
