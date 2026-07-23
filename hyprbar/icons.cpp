// hyprbar/icons.cpp — icon loading and resolution: GTK theme dirs, PNG/SVG, per-use caches

#include "common/icons.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- icons (tasklist + tray) ----

    SP<ITexture> loadPng(const std::string& path) {
        auto* SURF = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // dbusmenu "icon-data" is a PNG shipped inline (nm-applet composites its
    // per-network signal-strength icons at runtime — they exist nowhere on
    // disk, so a theme lookup can never find them).
    SP<ITexture> loadPngBytes(const std::vector<uint8_t>& data) {
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
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // SVG -> texture via librsvg (alacritty ships nothing but an SVG; tray icon
    // names like input-keyboard-symbolic only exist as theme SVGs). Rasterized
    // bigger than any bar cell; GL scales down. Symbolic icons are a pure alpha
    // shape in some theme-chosen color — repaint them with the bar's fg.
    static SP<ITexture> loadSvg(const std::string& path, bool recolor, const CHyprColor& col) {
        RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), nullptr);
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

    static SP<ITexture> loadIcon(const std::string& path) {
        if (path.ends_with(".svg")) {
            const bool SYMBOLIC = path.find("-symbolic") != std::string::npos || path.find("/symbolic/") != std::string::npos;
            return loadSvg(path, SYMBOLIC, color(cfg.colFg));
        }
        return loadPng(path);
    }

    // Icon name (or absolute path) -> a file on disk. The freedesktop
    // name walk lives in common/icons.hpp (shared with hyprnotify's cards);
    // this wrapper adds what the bar needs on top: a tray item's own
    // IconThemePath dir first, and a lowercase retry for window classes
    // that don't match their icon name's case.
    std::string resolveIconPath(const std::string& name, const std::string& extraDir) {
        if (name.empty())
            return "";
        std::error_code ec;
        if (name.front() == '/')
            return std::filesystem::exists(name, ec) ? name : "";

        if (!extraDir.empty())
            for (const auto& N : {name, lower(name)})
                for (const char* EXT : {".png", ".svg"}) {
                    const auto P = extraDir + "/" + N + EXT;
                    if (std::filesystem::exists(P, ec))
                        return P;
                }

        if (auto P = NHyprCommon::resolveIconName(name, 48); !P.empty())
            return P;
        return NHyprCommon::resolveIconName(lower(name), 48);
    }

    // Window class -> Icon= from the app's .desktop file.
    static std::string desktopIconName(const std::string& klass) {
        for (const auto& N : {klass, lower(klass)}) {
            std::ifstream F("/usr/share/applications/" + N + ".desktop");
            if (!F)
                continue;
            std::string line;
            while (std::getline(F, line)) {
                if (line.starts_with("Icon="))
                    return line.substr(5);
            }
        }
        return "";
    }

    // symbolic SVGs bake col_fg into their pixels (loadIcon), so a foreground
    // change invalidates every icon cache — checked at the caches' entrances
    static void dropStaleTint();

    // class -> texture; nullptr is cached too (= use the letter fallback).
    static std::unordered_map<std::string, SP<ITexture>> appIconCache;

    SP<ITexture>                                         appIcon(const std::string& klass) {
        if (klass.empty())
            return nullptr;
        dropStaleTint();
        if (const auto IT = appIconCache.find(klass); IT != appIconCache.end())
            return IT->second;

        if (!warmGate.mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        auto path = resolveIconPath(klass);
        if (path.empty())
            path = resolveIconPath(desktopIconName(klass));

        SP<ITexture> tex    = path.empty() ? nullptr : loadIcon(path);
        appIconCache[klass] = tex;
        return tex;
    }

    // icon name or absolute path (a .desktop Icon= value) -> texture; nullptr cached too
    static std::unordered_map<std::string, SP<ITexture>> namedIconCache;

    SP<ITexture>                                         namedIcon(const std::string& name) {
        if (name.empty())
            return nullptr;
        dropStaleTint();
        if (const auto IT = namedIconCache.find(name); IT != namedIconCache.end())
            return IT->second;

        if (!warmGate.mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        const auto   path    = resolveIconPath(name);
        SP<ITexture> tex     = path.empty() ? nullptr : loadIcon(path);
        namedIconCache[name] = tex;
        return tex;
    }

    // tray icon name (+ the item's own theme dir) -> texture; nullptr cached
    // too. fcitx REALLY flips its icon on every IM toggle / input context
    // change — without this cache every flip re-resolved and re-rasterized the
    // file from disk inside the render pass.
    static std::unordered_map<std::string, SP<ITexture>> trayIconCache;

    static void                                          dropStaleTint() {
        static uint64_t lastFg = 0;
        const auto      FG     = (uint64_t)cfg.colFg->value();
        if (FG == lastFg)
            return;
        lastFg = FG;
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
    }

    SP<ITexture> trayIcon(const std::string& name, const std::string& themePath) {
        const auto KEY = name + "|" + themePath;
        dropStaleTint();
        if (const auto IT = trayIconCache.find(KEY); IT != trayIconCache.end())
            return IT->second;

        if (!warmGate.mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        const auto   path  = resolveIconPath(name, themePath);
        SP<ITexture> tex   = path.empty() ? nullptr : loadIcon(path);
        trayIconCache[KEY] = tex;
        return tex;
    }

    void iconsExit() {
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
    }

} // namespace NHyprbar
