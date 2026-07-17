// hyprnotify/icons.cpp — notification images: files via hyprgraphics, raw image-data

#include "hyprnotify.hpp"

namespace NHyprnotify {

    // Big photos (the OSD's icon set is arbitrary user images) rasterized
    // whole would upload megapixels the card shows at <=100 logical px —
    // downscale on the CPU once instead, at load time.
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

        if (SZ.x <= maxPx && SZ.y <= maxPx)
            return g_pHyprRenderer->createTexture(SURF->cairo());

        const double SCALE = std::min(maxPx / SZ.x, maxPx / SZ.y);
        const int    W     = std::max(1, (int)std::lround(SZ.x * SCALE));
        const int    H     = std::max(1, (int)std::lround(SZ.y * SCALE));

        auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR    = cairo_create(SMALL);
        cairo_scale(CR, W / SZ.x, H / SZ.y);
        cairo_set_source_surface(CR, SURF->cairo(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(SMALL);

        auto tex = g_pHyprRenderer->createTexture(SMALL);
        cairo_surface_destroy(SMALL);
        return tex;
    }

    void ensureIconTex(SNotif& n, int maxPx) {
        if (!n.pixels.empty()) {
            if (n.iconTex && n.pixelsFor == n.pixels)
                return;
            n.iconTex   = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, n.pixels.data(), n.pw * 4, Vector2D{(double)n.pw, (double)n.ph});
            n.pixelsFor = n.pixels;
            n.imageFor.clear();
            return;
        }
        n.pixelsFor.clear();

        if (n.image.empty()) {
            n.iconTex.reset();
            n.imageFor.clear();
            return;
        }
        if (n.imageFor == n.image) // also remembers a failed load: no disk retry per warm
            return;
        n.iconTex  = fileTex(n.image, maxPx);
        n.imageFor = n.image;
    }

} // namespace NHyprnotify
