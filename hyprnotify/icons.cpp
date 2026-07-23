// hyprnotify/icons.cpp — notification images: content avatars and identity
// icons via hyprgraphics, raw image-data pixmaps

#include "common/icons.hpp"

#include "hyprnotify.hpp"

#include <cstring>
#include <filesystem>

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

    // Scale into exactly W x H, covering the box: the overflowing axis is
    // center-cropped (the hero treatment for previews). When the source
    // aspect matches, cover == fit and nothing is lost.
    static SP<ITexture> coverTex(cairo_surface_t* src, double sw, double sh, int W, int H) {
        auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto*        CR    = cairo_create(SMALL);
        const double S     = std::max(W / sw, H / sh);
        cairo_translate(CR, (W - sw * S) / 2.0, (H - sh * S) / 2.0);
        cairo_scale(CR, S, S);
        cairo_set_source_surface(CR, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
        cairo_paint(CR);
        cairo_destroy(CR);
        cairo_surface_flush(SMALL);

        auto tex = g_pHyprRenderer->createTexture(SMALL);
        cairo_surface_destroy(SMALL);
        return tex;
    }

    // A source wide enough for the hero layout: HERO_ASPECT and at least
    // half the hero box, so a tiny wide icon never blows up to card width.
    static bool heroWorthy(double sw, double sh, int heroWPx) {
        return heroWPx > 0 && sh > 0 && sw / sh >= HERO_ASPECT && sw * 2 >= heroWPx;
    }

    // CImage's size hint only bounds SVG rasters; raster formats decode full
    // size transiently and get scaled here.
    static SP<ITexture> fileTex(const std::string& path, int iconPx, int heroWPx, int heroHCapPx, bool& hero) {
        const int            HINT = std::max(iconPx, heroWPx);
        Hyprgraphics::CImage image(path, Vector2D{(double)HINT, (double)HINT});
        if (!image.success())
            return nullptr;

        const auto SURF = image.cairoSurface();
        if (!SURF || SURF->status() != CAIRO_STATUS_SUCCESS)
            return nullptr;

        const auto SZ = SURF->size();
        if (SZ.x <= 0 || SZ.y <= 0)
            return nullptr;

        hero = heroWorthy(SZ.x, SZ.y, heroWPx);
        if (hero)
            return coverTex(SURF->cairo(), SZ.x, SZ.y, heroWPx, std::min((int)std::lround(heroWPx * SZ.y / SZ.x), heroHCapPx));
        return scaledTex(SURF->cairo(), SZ.x, SZ.y, iconPx);
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

    // The icon anatomy: n.iconTex carries the CONTENT (image-data pixmap or
    // image-path file, hero-capable), n.identTex the IDENTITY (app_icon /
    // desktop-entry, icon-box only). The render decides which leads and
    // whether the identity rides as the corner badge.
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx) {
        // identity: one raster per source, drawn at every size it appears
        // (lead icon, group header, the 13px badge)
        if (n.identity.empty()) {
            n.identTex.reset();
            n.identFor.clear();
        } else if (n.identFor != n.identity) { // remembers a failed load too: no disk retry per warm
            bool hero  = false;
            n.identTex = fileTex(n.identity, iconPx, 0, 0, hero);
            n.identFor = n.identity;
        }

        if (n.hasPixels) {
            if (n.pixels.empty())
                return; // uploaded by an earlier warm; the texture carries it now

            uint64_t h = fnv1a(n.pixels.data(), n.pixels.size(), 0xcbf29ce484222325ULL);
            h          = fnv1a(&n.pw, sizeof(n.pw), h);
            h          = fnv1a(&n.ph, sizeof(n.ph), h);
            if (!n.iconTex || n.pixelsFor != h) {
                n.heroTex = heroWorthy(n.pw, n.ph, heroWPx);
                if (n.heroTex || n.pw > iconPx || n.ph > iconPx) {
                    // stride pw*4 is how unpackImageData lays the buffer out
                    auto* SRC = cairo_image_surface_create_for_data(n.pixels.data(), CAIRO_FORMAT_ARGB32, n.pw, n.ph, n.pw * 4);
                    if (cairo_surface_status(SRC) != CAIRO_STATUS_SUCCESS)
                        n.iconTex = nullptr;
                    else if (n.heroTex)
                        n.iconTex = coverTex(SRC, n.pw, n.ph, heroWPx, std::min((int)std::lround((double)heroWPx * n.ph / n.pw), heroHCapPx));
                    else
                        n.iconTex = scaledTex(SRC, n.pw, n.ph, iconPx);
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
            // no content image: the identity leads alone (render side), or
            // the card is text-only — never a rolled fallback (retired 4.0.0)
            n.iconTex.reset();
            n.imageFor.clear();
            n.pixelsFor = 0;
            n.heroTex   = false;
            return;
        }
        if (n.imageFor == n.image) // also remembers a failed load: no disk retry per warm
            return;
        bool hero   = false;
        n.iconTex   = fileTex(n.image, iconPx, heroWPx, heroHCapPx, hero);
        n.imageFor  = n.image;
        n.pixelsFor = 0;
        n.heroTex   = hero;
    }

    void ensureActionIcon(SNotif& n, SAction& a, int iconPx) {
        if (!n.actionIcons) {
            a.iconTex.reset();
            a.iconFor.clear(); // so re-enabling the hint rebuilds
            return;
        }
        if (a.iconFor == a.id) // also remembers a failed resolve: no rescan per warm
            return;
        a.iconFor = a.id;
        a.iconTex.reset();

        std::string path = a.id;
        if (path.starts_with("file://"))
            path.erase(0, 7);
        if (!path.starts_with('/'))
            path = NHyprCommon::resolveIconName(a.id, iconPx);
        if (path.empty())
            return;
        bool hero = false;
        a.iconTex = fileTex(path, iconPx, 0, 0, hero); // icon box only, never hero
    }

    void ensureBodyImage(SBodyImage& im, int maxPx) {
        if (im.src.empty()) {
            im.tex.reset();
            im.builtFor.clear();
            return;
        }
        if (im.builtFor == im.src) // remembers a failed load too: no disk retry per warm
            return;
        im.builtFor = im.src;
        bool hero   = false;
        im.tex      = fileTex(im.src, maxPx, 0, 0, hero); // fit a box, never hero
    }

} // namespace NHyprnotify
