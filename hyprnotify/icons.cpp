// hyprnotify/icons.cpp — notification images: files via hyprgraphics, raw image-data

#include "hyprnotify.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace NHyprnotify {

    // ---- fallback_icon_dir: iconless cards get a face ----

    static std::vector<std::string> fallbackFiles;
    static bool                     fallbackScanned = false;

    void                            resetFallbackCache() {
        fallbackFiles.clear();
        fallbackScanned = false;
    }

    // One roll per card (bus keeps the pick across in-place replaces). The
    // listing is scanned once per config life, from the warm pass — never
    // the render or a bus dispatch.
    static std::string pickFallback() {
        const auto DIR = cfg.fallbackIconDir->value();
        if (DIR.empty())
            return "";
        if (!fallbackScanned) {
            fallbackScanned = true;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(DIR, std::filesystem::directory_options::skip_permission_denied, ec); !ec && it != std::filesystem::end(it);
                 it.increment(ec)) {
                if (!it->is_regular_file(ec))
                    continue;
                auto ext = it->path().extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".avif" || ext == ".jxl" || ext == ".svg")
                    fallbackFiles.push_back(it->path().string());
            }
        }
        if (fallbackFiles.empty())
            return "";
        static std::mt19937 rng{std::random_device{}()};
        return fallbackFiles[std::uniform_int_distribution<size_t>{0, fallbackFiles.size() - 1}(rng)];
    }

    // ---- freedesktop icon-name resolution ----

    // A name -> path map. A miss is cached too (empty string) so a nonexistent
    // name never rescans the theme every warm. Cleared on config reload.
    static std::unordered_map<std::string, std::string> iconPathCache;

    void                                                resetIconThemeCache() {
        iconPathCache.clear();
    }

    // The GTK icon theme is this system's source of truth (Qt follows it). Read
    // gtk-icon-theme-name from settings.ini once; fall back to hicolor.
    static std::string gtkIconTheme() {
        static std::string theme;
        static bool        done = false;
        if (done)
            return theme;
        done = true;

        std::string cfgHome;
        if (const char* X = getenv("XDG_CONFIG_HOME"); X && *X)
            cfgHome = X;
        else if (const char* H = getenv("HOME"); H && *H)
            cfgHome = std::string(H) + "/.config";
        if (!cfgHome.empty()) {
            std::ifstream f(cfgHome + "/gtk-3.0/settings.ini");
            std::string   line;
            while (std::getline(f, line))
                if (const auto P = line.find("gtk-icon-theme-name"); P == 0) {
                    if (const auto EQ = line.find('='); EQ != std::string::npos) {
                        theme = line.substr(EQ + 1);
                        theme.erase(0, theme.find_first_not_of(" \t"));
                        theme.erase(theme.find_last_not_of(" \t\r\n") + 1);
                    }
                    break;
                }
        }
        return theme;
    }

    // Look for <dir>/name.ext directly and one category level down
    // (<dir>/<cat>/name.ext) — the freedesktop size dirs hold either.
    static std::string findInDir(const std::string& dir, const std::string& name) {
        static const char* EXT[] = {".svg", ".png", ".xpm"};
        std::error_code    ec;
        for (const char* e : EXT)
            if (std::filesystem::exists(dir + "/" + name + e, ec))
                return dir + "/" + name + e;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::end(it); it.increment(ec)) {
            if (!it->is_directory(ec))
                continue;
            for (const char* e : EXT)
                if (std::filesystem::exists(it->path().string() + "/" + name + e, ec))
                    return it->path().string() + "/" + name + e;
        }
        return "";
    }

    // Not a full index.theme engine: it scans the GTK theme's size dirs
    // (scalable first, then a size-proximity order), then hicolor, then flat
    // pixmaps. Inheritance beyond hicolor isn't followed — app icons live in
    // hicolor in practice, so that fallback covers the common miss.
    std::string resolveIconName(const std::string& name, int sizePx) {
        if (name.empty() || name.find('/') != std::string::npos)
            return ""; // already a path, or nothing to resolve
        if (const auto IT = iconPathCache.find(name); IT != iconPathCache.end())
            return IT->second;

        std::vector<std::string> bases;
        if (const char* X = getenv("XDG_DATA_HOME"); X && *X)
            bases.push_back(std::string(X) + "/icons");
        else if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.local/share/icons");
        if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.icons");
        std::string dataDirs = "/usr/local/share:/usr/share";
        if (const char* X = getenv("XDG_DATA_DIRS"); X && *X)
            dataDirs = X;
        for (size_t p = 0; p < dataDirs.size();) {
            const auto E = dataDirs.find(':', p);
            const auto D = dataDirs.substr(p, E == std::string::npos ? E : E - p);
            if (!D.empty())
                bases.push_back(D + "/icons");
            if (E == std::string::npos)
                break;
            p = E + 1;
        }

        std::vector<std::string> themes;
        if (const auto GT = gtkIconTheme(); !GT.empty())
            themes.push_back(GT);
        themes.push_back("hicolor");

        std::vector<std::string> sizeDirs = {"scalable"};
        for (const int S : {sizePx, 64, 48, 96, 128, 256, 72, 32, 24, 16})
            sizeDirs.push_back(std::to_string(S) + "x" + std::to_string(S));

        std::string found;
        for (const auto& THEME : themes) {
            for (const auto& BASE : bases) {
                const auto      TDIR = BASE + "/" + THEME;
                std::error_code ec;
                if (!std::filesystem::is_directory(TDIR, ec))
                    continue;
                for (const auto& SD : sizeDirs)
                    if (found = findInDir(TDIR + "/" + SD, name); !found.empty())
                        break;
                if (!found.empty())
                    break;
            }
            if (!found.empty())
                break;
        }
        if (found.empty())
            for (const char* e : {".svg", ".png", ".xpm"}) {
                std::error_code ec;
                if (std::filesystem::exists(std::string("/usr/share/pixmaps/") + name + e, ec)) {
                    found = std::string("/usr/share/pixmaps/") + name + e;
                    break;
                }
            }

        iconPathCache[name] = found;
        return found;
    }

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

    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx) {
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
            // no image sent: the card draws its rolled fallback face (icon
            // treatment always — a wide waifu must not go hero)
            if (n.fallbackPick.empty())
                n.fallbackPick = pickFallback();
            if (n.fallbackPick.empty()) {
                n.iconTex.reset();
                n.imageFor.clear();
                n.pixelsFor = 0;
                n.heroTex   = false;
                return;
            }
            if (n.imageFor == n.fallbackPick)
                return;
            bool hero   = false;
            n.iconTex   = fileTex(n.fallbackPick, iconPx, 0, 0, hero);
            n.imageFor  = n.fallbackPick;
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
            path = resolveIconName(a.id, iconPx);
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
