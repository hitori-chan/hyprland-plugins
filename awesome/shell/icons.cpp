// awesome/shell/icons.cpp — icon loading and resolution: GTK theme dirs, PNG/SVG, per-use caches

#include "../core/icons.hpp" // the shared XDG walk + the GTK theme name
#include "../core/desktopindex.hpp"

#include "shell/shell.hpp"

namespace NAwesome::Shell {

    // ---- icons (tasklist + tray) ----

    // PNG bytes -> a texture: the declared size checked before cairo decodes
    // the whole raster, and a large icon downscaled once on the CPU (a bar
    // cell is <=40 px; apps that ship a single 256-1024 px icon must not
    // ride to GL full size — the notify module's scaledTex rule)
    static SP<ITexture> pngTexture(const std::vector<uint8_t>& data, uint32_t maxDim) {
        const auto DIMS = NAwesome::rasterDims(data.data(), data.size());
        if (!DIMS || DIMS->first == 0 || DIMS->second == 0 || DIMS->first > maxDim || DIMS->second > maxDim ||
            (uint64_t)DIMS->first * DIMS->second > NAwesome::MAX_DECODE_PIXELS)
            return nullptr;

        struct SCursor {
            const uint8_t* p;
            size_t         left;
        } cur{data.data(), data.size()};
        const auto READ = [](void* closure, unsigned char* out, unsigned int len) -> cairo_status_t {
            auto* C = (SCursor*)closure;
            if (C->left < len)
                return CAIRO_STATUS_READ_ERROR;
            std::copy_n(C->p, len, out);
            C->p += len;
            C->left -= len;
            return CAIRO_STATUS_SUCCESS;
        };
        auto* SURF = cairo_image_surface_create_from_png_stream(READ, &cur);
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        const int W = cairo_image_surface_get_width(SURF), H = cairo_image_surface_get_height(SURF);
        if (W <= 0 || H <= 0 || (uint32_t)W > maxDim || (uint32_t)H > maxDim) { // what cairo decoded must be what was declared
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        constexpr int CAP = 128;
        if (W > CAP || H > CAP) {
            const double SCALE = std::min((double)CAP / W, (double)CAP / H);
            const int    DW = std::max(1, (int)std::lround(W * SCALE)), DH = std::max(1, (int)std::lround(H * SCALE));
            auto*        SMALL = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DW, DH);
            auto*        CR    = cairo_create(SMALL);
            cairo_scale(CR, (double)DW / W, (double)DH / H);
            cairo_set_source_surface(CR, SURF, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
            cairo_paint(CR);
            cairo_destroy(CR);
            cairo_surface_flush(SMALL);
            cairo_surface_destroy(SURF);
            SURF = SMALL;
        }
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // Decodes run on the compositor thread, inside a warm: a 1024² PNG is
    // ~20 ms, a large SVG as much. Past this many LARGE decodes (declared
    // past 256² px, an SVG past 64 KiB) a window, the rest wait — deferred
    // like a refused theme walk (uncached, retried) — so a bar of large
    // icons fills over a few frames instead of stalling one. Small icons,
    // the theme's usual 16-48 px, are never held back.
    constexpr int  MAX_LARGE_DECODES_PER_WINDOW = 2;
    constexpr auto LARGE_DECODE_WINDOW          = std::chrono::milliseconds(250);
    static bool    largeDecodeBudget() {
        static auto WINDOW = std::chrono::steady_clock::now();
        static int  used   = 0;
        const auto  NOW    = std::chrono::steady_clock::now();
        if (NOW - WINDOW >= LARGE_DECODE_WINDOW) {
            WINDOW = NOW;
            used   = 0;
        }
        if (used < MAX_LARGE_DECODES_PER_WINDOW) {
            used++;
            return true;
        }
        NAwesome::iconDeferred() = true;
        return false;
    }

    // Every decode here runs on the compositor thread, the small ones too: a
    // launcher's first open decoded its 12-20 visible icons in one warm, a
    // tray menu up to 512 named ones in one reply. Past DECODE_BUDGET of
    // decode time in a DECODE_WINDOW the rest defer like a refused walk
    // (uncached, retried a beat later), so they fill in over a few frames.
    constexpr auto                        DECODE_BUDGET = std::chrono::microseconds(4000);
    constexpr auto                        DECODE_WINDOW = std::chrono::milliseconds(16);
    static std::chrono::steady_clock::time_point decodeWindow;
    static std::chrono::nanoseconds       decodeSpent{0};
    static bool                           decodeRefused = false; // the retry comes back a window later, not a walk's
    static bool                           decodeBudgetOpen() {
        const auto NOW = std::chrono::steady_clock::now();
        if (NOW - decodeWindow >= DECODE_WINDOW) {
            decodeWindow = NOW;
            decodeSpent  = {};
        }
        if (decodeSpent < DECODE_BUDGET)
            return true;
        NAwesome::iconDeferred() = true;
        decodeRefused            = true;
        return false;
    }
    template <class FN>
    static SP<ITexture> timedDecode(FN&& fn) {
        const auto T0  = std::chrono::steady_clock::now();
        auto       TEX = fn();
        decodeSpent += std::chrono::steady_clock::now() - T0;
        return TEX;
    }

    SP<ITexture> loadPng(const std::string& path) {
        // the bytes come from ONE non-blocking open (core/icons.hpp): an
        // absolute SNI IconName reaches here. A bar cell is <=40 px: 1024 a
        // side is room for any app's largest icon, at a bounded decode.
        const auto BYTES = NAwesome::readImageFile(path);
        if (!BYTES)
            return nullptr;
        if (const auto DIMS = NAwesome::rasterDims(BYTES->data(), BYTES->size()); DIMS && (uint64_t)DIMS->first * DIMS->second > 256 * 256 && !largeDecodeBudget())
            return nullptr;
        return pngTexture(*BYTES, 1024);
    }

    // dbusmenu "icon-data" is a PNG shipped inline (nm-applet composites its
    // per-network signal-strength icons at runtime — they exist nowhere on
    // disk, so a theme lookup can never find them). A menu row's icon is
    // ~16-24 px: the declared size is held to 256 px a side before cairo
    // decodes it (a 4096² PNG of one color is a few KB on the wire and
    // 64 MiB decoded).
    SP<ITexture> loadPngBytes(const std::vector<uint8_t>& data) {
        constexpr size_t MAX_ENCODED = 4u << 20;
        if (data.empty() || data.size() > MAX_ENCODED)
            return nullptr;
        // charged, never refused: an inline icon has no name to retry by
        return timedDecode([&] { return pngTexture(data, 256); });
    }

    // SVG -> texture via librsvg (alacritty ships nothing but an SVG; tray icon
    // names like input-keyboard-symbolic only exist as theme SVGs). Rasterized
    // bigger than any bar cell; GL scales down. Symbolic icons are a pure alpha
    // shape in some theme-chosen color — repaint them with the bar's fg.
    static SP<ITexture> loadSvg(const std::string& path, bool recolor, const CHyprColor& col) {
        const auto BYTES = NAwesome::readImageFile(path); // one non-blocking open, size-bounded
        if (!BYTES || (BYTES->size() > (64u << 10) && !largeDecodeBudget()))
            return nullptr;
        RsvgHandle* handle = rsvg_handle_new_from_data(BYTES->data(), BYTES->size(), nullptr);
        if (!handle)
            return nullptr;

        constexpr int SZ   = 64;
        auto*         SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SZ, SZ);
        auto*         CR   = cairo_create(SURF);
        RsvgRectangle viewport{0, 0, SZ, SZ};
        rsvg_handle_render_document(handle, CR, &viewport, nullptr);
        cairo_destroy(CR);
        g_object_unref(handle);
        cairo_surface_flush(SURF);

        if (recolor) {
            // cairo ARGB32 is premultiplied, native-endian (B,G,R,A on x86)
            const uint8_t R = (uint8_t)(col.r * 255), G = (uint8_t)(col.g * 255), B = (uint8_t)(col.b * 255);
            auto*         D      = cairo_image_surface_get_data(SURF);
            const int     STRIDE = cairo_image_surface_get_stride(SURF);
            for (int y = 0; y < SZ; y++) {
                auto* row = D + (size_t)y * STRIDE;
                for (int x = 0; x < SZ; x++) {
                    const uint8_t A = row[x * 4 + 3];
                    row[x * 4]      = (uint8_t)(B * A / 255);
                    row[x * 4 + 1]  = (uint8_t)(G * A / 255);
                    row[x * 4 + 2]  = (uint8_t)(R * A / 255);
                }
            }
            cairo_surface_mark_dirty(SURF);
        }

        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return tex;
    }

    static SP<ITexture> loadIconNow(const std::string& path);
    static SP<ITexture> loadIcon(const std::string& path) {
        if (!decodeBudgetOpen())
            return nullptr;
        return timedDecode([&] { return loadIconNow(path); });
    }

    static SP<ITexture> loadIconNow(const std::string& path) {
        if (path.ends_with(".svg")) {
            const bool SYMBOLIC = path.find("-symbolic") != std::string::npos || path.find("/symbolic/") != std::string::npos;
            return loadSvg(path, SYMBOLIC, color(cfg().getColor("plugin:awesome:shell:col_fg")));
        }
        return loadPng(path);
    }

    static std::unordered_map<std::string, SP<ITexture>> appIconCache;
    static std::unordered_map<std::string, SP<ITexture>> namedIconCache, trayIconCache;

    // Icon name (or absolute path) -> a file on disk, PNG or SVG: an SNI's
    // own IconThemePath first, then the core's one freedesktop resolver —
    // the bar and the cards resolve a name to the same file, through the
    // same plausibility gate and bounded memo.
    std::string resolveIconPath(const std::string& name, const std::string& extraDir) {
        if (name.empty())
            return "";
        if (name.front() == '/')
            return NAwesome::admissibleImageFile(name) ? name : "";
        if (!NAwesome::plausibleIconName(name))
            return "";

        if (!extraDir.empty())
            for (const auto& N : {name, lower(name)})
                for (const char* EXT : {".png", ".svg"})
                    if (const auto P = extraDir + "/" + N + EXT; NAwesome::admissibleImageFile(P))
                        return P;

        constexpr int BAR_ICON_PX = 24; // the strip's icon cells, the size a lookup prefers
        if (auto P = NAwesome::resolveIconName(name, BAR_ICON_PX); !P.empty())
            return P;
        if (const auto LOWER = lower(name); LOWER != name)
            return NAwesome::resolveIconName(LOWER, BAR_ICON_PX);
        return "";
    }

    // Window class -> Icon= from the app's .desktop file. A window's class
    // rarely equals its desktop-file basename (ente's class is "ente" but it
    // ships ente-desktop.desktop; qBittorrent's is "qbittorrent" under
    // org.qbittorrent.qBittorrent.desktop) — the spec's StartupWMClass field
    // is the canonical association, so scan for it when the basename misses.
    static std::string desktopIconName(const std::string& identifier) {
        const auto& INDEX = NAwesome::CDesktopIndex::inst();
        const auto  ID    = lower(identifier);
        if (const auto* ICON = INDEX.findIcon(ID))
            return *ICON;

        // SNI Id is stable but not required to be a desktop-file id. Accept a
        // mapped desktop identity only at an explicit suffix boundary, so an
        // id such as app_status_icon can recover "app" without letting "app"
        // match an unrelated "application" id.
        const std::string* best = nullptr;
        size_t             bestLength = 0;
        for (const auto& [key, icon] : INDEX.icons()) {
            if (key.size() <= bestLength || !NAwesome::iconIdentityPrefixMatch(ID, key))
                continue;
            best       = &icon;
            bestLength = key.size();
        }
        return best ? *best : "";
    }

    // The caches key on strings a local client can rotate (window class,
    // dbusmenu icon-name, the SNI IconName|ThemePath|Id): one chattering
    // client must not grow them without bound. At the cap, drop everything —
    // the defined behavior — and let the next warm rebuild what is still
    // wanted.
    constexpr size_t MAX_ICON_CACHE = 512;
    template <class MAP>
    static void boundIconCache(MAP& m) {
        if (m.size() >= MAX_ICON_CACHE)
            m.clear();
    }

    // A deferred lookup (core: iconDeferred) is not a miss: it stays out of
    // the caches, and one warm a beat later picks it up — when the budgets
    // have room again.
    static SP<CEventLoopTimer> retry;
    template <class MAP>
    static SP<ITexture> remember(MAP& m, const std::string& key, SP<ITexture> tex) {
        if (!tex && NAwesome::iconDeferred()) {
            if (retry && !retry->armed())
                retry->updateTimeout(decodeRefused ? std::chrono::milliseconds(20) : std::chrono::milliseconds(300));
            decodeRefused = false;
            return nullptr;
        }
        boundIconCache(m);
        m[key] = tex;
        return tex;
    }

    // symbolic SVGs bake col_fg into their pixels (loadIcon), so a foreground
    // change invalidates every icon cache — checked at the caches' entrances
    static void dropStaleTint();

    // class -> texture; nullptr is cached too (= leave the icon cell empty).
    SP<ITexture>                                         appIcon(const std::string& klass) {
        if (klass.empty())
            return nullptr;
        dropStaleTint();
        if (const auto IT = appIconCache.find(klass); IT != appIconCache.end())
            return IT->second;

        if (!warmGate().mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        NAwesome::iconDeferred() = false;
        auto path = resolveIconPath(klass);
        if (path.empty())
            path = resolveIconPath(desktopIconName(klass));
        return remember(appIconCache, klass, path.empty() ? nullptr : loadIcon(path));
    }

    // icon name or absolute path (a .desktop Icon= value) -> texture; nullptr cached too
    SP<ITexture>                                         namedIcon(const std::string& name) {
        if (name.empty())
            return nullptr;
        dropStaleTint();
        if (const auto IT = namedIconCache.find(name); IT != namedIconCache.end())
            return IT->second;

        if (!warmGate().mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        NAwesome::iconDeferred() = false;
        const auto path = resolveIconPath(name);
        return remember(namedIconCache, name, path.empty() ? nullptr : loadIcon(path));
    }

    // tray icon name (+ the item's own theme dir) -> texture; nullptr cached
    // too. fcitx REALLY flips its icon on every IM toggle / input context
    // change — without this cache every flip re-resolved and re-rasterized the
    // file from disk inside the render pass.
    static void                                          dropStaleTint() {
        static uint64_t lastFg = 0;
        const auto      FG     = (uint64_t)cfg().getColor("plugin:awesome:shell:col_fg");
        if (FG == lastFg)
            return;
        lastFg = FG;
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
        // the tray cells hold textures from the dropped cache: they kept the
        // old tint until their app changed its icon
        if (!Tray::items.empty()) {
            for (const auto& IT : Tray::items)
                IT->dirty = true;
            barChanged(); // a deferred rewarm: this runs inside one
        }
    }

    SP<ITexture> trayIcon(const std::string& name, const std::string& themePath, const std::string& id) {
        const auto KEY = name + "|" + themePath + "|" + id;
        dropStaleTint();
        if (const auto IT = trayIconCache.find(KEY); IT != trayIconCache.end())
            return IT->second;

        if (!warmGate().mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        NAwesome::iconDeferred() = false;
        auto path = resolveIconPath(name, themePath);
        if (path.empty())
            path = resolveIconPath(desktopIconName(name), themePath);
        if (path.empty())
            path = resolveIconPath(id, themePath);
        if (path.empty())
            path = resolveIconPath(desktopIconName(id), themePath);
        return remember(trayIconCache, KEY, path.empty() ? nullptr : loadIcon(path));
    }

    // A reload may follow a GTK theme switch, and everything above was
    // resolved against the old one: then drop the caches; the next warm
    // resolves again. The same theme keeps what it resolved (a reload per
    // config save re-decoded every icon) and drops only the misses, so an
    // icon installed since can still land.
    static uint64_t seenThemeGeneration = 0;
    bool            iconsReload() {
        NAwesome::refreshIconTheme();
        if (NAwesome::iconThemeGeneration() == seenThemeGeneration) {
            std::erase_if(appIconCache, [](const auto& E) { return !E.second; });
            std::erase_if(namedIconCache, [](const auto& E) { return !E.second; });
            std::erase_if(trayIconCache, [](const auto& E) { return !E.second; });
            return false;
        }
        seenThemeGeneration = NAwesome::iconThemeGeneration();
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
        return true;
    }

    void iconsInit() {
        NAwesome::refreshIconTheme();
        seenThemeGeneration = NAwesome::iconThemeGeneration();
        retry = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { barChanged(); }, nullptr);
        g_pEventLoopManager->addTimer(retry);
        // the shared .desktop index (core) grew: new entries can only fill
        // MISSES (cached nulls) — a resolved icon stays resolved; clearing
        // everything per batch re-decoded every task and tray icon dozens of
        // times while the index loaded
        NAwesome::CDesktopIndex::inst().subscribe([]() {
            std::erase_if(appIconCache, [](const auto& E) { return !E.second; });
            std::erase_if(trayIconCache, [](const auto& E) { return !E.second; });
            for (const auto& item : Tray::items)
                if (item->pixels.empty() && (!item->tex || item->tex->m_texID == 0))
                    item->dirty = true;
            barChanged();
        });
    }

    void iconsExit() {
        if (retry && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(retry);
        retry.reset();
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
    }

} // namespace NAwesome::Shell
