// hyprbar/icons.cpp — icon loading and resolution: GTK theme dirs, PNG/SVG, per-use caches

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

    std::string lower(std::string s) {
        for (auto& c : s)
            c = std::tolower((unsigned char)c);
        return s;
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

    // Where icons live, probed once at init (existing dirs only). Not a full
    // xdg icon-theme implementation — a fixed, ordered list: the user's GTK
    // theme (both dir layouts), hicolor, pixmaps, Adwaita's symbolic set as
    // the last resort for freedesktop names like input-keyboard-symbolic.
    static std::vector<std::string> iconDirs;

    static std::string              gtkIconTheme() {
        const char* HOME = std::getenv("HOME");
        if (!HOME)
            return "";
        std::ifstream F(std::string{HOME} + "/.config/gtk-3.0/settings.ini");
        std::string   line;
        while (F && std::getline(F, line)) {
            if (line.starts_with("gtk-icon-theme-name=")) {
                auto v = line.substr(20);
                while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
                    v.pop_back();
                return v;
            }
        }
        return "";
    }

    void buildIconDirs() {
        iconDirs.clear();
        std::error_code ec;
        const auto      add = [&](const std::string& d) {
            if (std::filesystem::is_directory(d, ec))
                iconDirs.push_back(d);
        };

        if (const auto THEME = gtkIconTheme(); !THEME.empty() && THEME != "hicolor") {
            const auto BASE = "/usr/share/icons/" + THEME + "/";
            for (const char* CTX : {"apps", "status", "devices", "categories"}) {
                for (const char* SZ : {"48", "32", "24", "22", "16"}) {
                    add(BASE + CTX + "/" + SZ);            // breeze layout
                    add(BASE + SZ + "x" + SZ + "/" + CTX); // classic layout
                }
                add(BASE + "scalable/" + CTX);
                add(BASE + "symbolic/" + CTX);
            }
        }
        for (const char* SZ : {"48x48", "64x64", "128x128", "32x32", "24x24", "22x22", "16x16"})
            add(std::string{"/usr/share/icons/hicolor/"} + SZ + "/apps");
        add("/usr/share/icons/hicolor/scalable/apps");
        add("/usr/share/pixmaps");
        for (const char* CTX : {"devices", "status", "apps", "legacy"})
            add(std::string{"/usr/share/icons/Adwaita/symbolic/"} + CTX);
    }

    // Icon name (or absolute path) -> a file on disk, PNG or SVG.
    std::string resolveIconPath(const std::string& name, const std::string& extraDir) {
        if (name.empty())
            return "";
        std::error_code ec;
        if (name.front() == '/')
            return std::filesystem::exists(name, ec) ? name : "";

        const auto tryDir = [&](const std::string& D) -> std::string {
            for (const auto& N : {name, lower(name)}) {
                for (const char* EXT : {".png", ".svg"}) {
                    const auto P = D + "/" + N + EXT;
                    if (std::filesystem::exists(P, ec))
                        return P;
                }
            }
            return "";
        };

        if (!extraDir.empty())
            if (auto P = tryDir(extraDir); !P.empty())
                return P;
        for (const auto& D : iconDirs)
            if (auto P = tryDir(D); !P.empty())
                return P;
        return "";
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

    // class -> texture; nullptr is cached too (= use the letter fallback).
    static std::map<std::string, SP<ITexture>> appIconCache;

    SP<ITexture>                               appIcon(const std::string& klass) {
        if (klass.empty())
            return nullptr;
        if (const auto IT = appIconCache.find(klass); IT != appIconCache.end())
            return IT->second;

        if (!warming) { // the texture rule (hyprbar.hpp): only the warm builds
            texStale = true;
            return nullptr;
        }

        auto path = resolveIconPath(klass);
        if (path.empty())
            path = resolveIconPath(desktopIconName(klass));

        SP<ITexture> tex    = path.empty() ? nullptr : loadIcon(path);
        appIconCache[klass] = tex;
        return tex;
    }

    // icon name or absolute path (a .desktop Icon= value) -> texture; nullptr cached too
    static std::map<std::string, SP<ITexture>> namedIconCache;

    SP<ITexture>                               namedIcon(const std::string& name) {
        if (name.empty())
            return nullptr;
        if (const auto IT = namedIconCache.find(name); IT != namedIconCache.end())
            return IT->second;

        if (!warming) { // the texture rule (hyprbar.hpp): only the warm builds
            texStale = true;
            return nullptr;
        }

        const auto   path    = resolveIconPath(name);
        SP<ITexture> tex     = path.empty() ? nullptr : loadIcon(path);
        namedIconCache[name] = tex;
        return tex;
    }

    // tray icon name (+ the item's own theme dir) -> texture; nullptr cached
    // too. fcitx REALLY flips its icon on every IM toggle / input context
    // change — without this cache every flip re-resolved and re-rasterized the
    // file from disk inside the render pass.
    static std::map<std::string, SP<ITexture>> trayIconCache;

    SP<ITexture>                               trayIcon(const std::string& name, const std::string& themePath) {
        const auto KEY = name + "|" + themePath;
        if (const auto IT = trayIconCache.find(KEY); IT != trayIconCache.end())
            return IT->second;

        if (!warming) { // the texture rule (hyprbar.hpp): only the warm builds
            texStale = true;
            return nullptr;
        }

        const auto   path  = resolveIconPath(name, themePath);
        SP<ITexture> tex   = path.empty() ? nullptr : loadIcon(path);
        trayIconCache[KEY] = tex;
        return tex;
    }

    void iconsExit() {
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
        iconDirs.clear();
    }

} // namespace NHyprbar
