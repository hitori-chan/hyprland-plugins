// hyprbar/icons.cpp — icon loading and resolution: GTK theme dirs, PNG/SVG, per-use caches

#include "common/fileindex.hpp"
#include "common/icons.hpp" // the shared XDG walk + the GTK theme name

#include "desktop_exec.hpp"
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
        constexpr size_t MAX_ENCODED = 4u << 20;
        constexpr int    MAX_DIM     = 2048;
        constexpr size_t MAX_PIXELS  = 4u << 20;
        if (data.empty() || data.size() > MAX_ENCODED)
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
        if (W <= 0 || H <= 0 || W > MAX_DIM || H > MAX_DIM || (size_t)W * (size_t)H > MAX_PIXELS) {
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

    // Where icons live, probed once at init (existing dirs only). Not a full
    // xdg icon-theme implementation — a fixed, ordered list of the dirs that
    // actually exist, so a lookup is a bounded stat walk instead of a theme
    // traversal: the user's GTK theme (both dir layouts), hicolor, pixmaps,
    // Adwaita's symbolic set as the last resort for freedesktop names like
    // input-keyboard-symbolic. Every tier walks the XDG bases in precedence
    // order — a theme (or an app icon) installed under ~/.local/share/icons
    // is as real as a packaged one.
    static std::vector<std::string> iconDirs;
    static std::unordered_map<std::string, SP<ITexture>> appIconCache;
    static std::unordered_map<std::string, SP<ITexture>> namedIconCache, trayIconCache;
    static NHyprCommon::CAsyncFileIndex                  desktopIndex;
    static SP<CEventLoopTimer>                           desktopPoll;
    static std::unordered_map<std::string, std::string>  desktopIcons;
    static uint64_t                                      desktopGeneration = 0;
    static bool                                          desktopScanning   = false;

    void                            buildIconDirs() {
        iconDirs.clear();
        std::error_code ec;
        const auto      add = [&](const std::string& d) {
            if (std::filesystem::is_directory(d, ec))
                iconDirs.push_back(d);
        };
        const auto BASES = NHyprCommon::xdgIconBases();

        // the GTK theme (Qt follows it) across every base, both layouts
        if (const auto THEME = NHyprCommon::gtkIconThemeName(); !THEME.empty() && THEME != "hicolor")
            for (const auto& B : BASES) {
                const auto TDIR = B + "/" + THEME + "/";
                for (const char* CTX : {"apps", "status", "devices", "categories"}) {
                    for (const char* SZ : {"48", "32", "24", "22", "16"}) {
                        add(TDIR + CTX + "/" + SZ);            // breeze layout
                        add(TDIR + SZ + "x" + SZ + "/" + CTX); // classic layout
                    }
                    add(TDIR + "scalable/" + CTX);
                    add(TDIR + "symbolic/" + CTX);
                }
            }
        for (const auto& B : BASES) {
            for (const char* SZ : {"48x48", "64x64", "128x128", "32x32", "24x24", "22x22", "16x16"})
                add(B + "/hicolor/" + SZ + "/apps");
            add(B + "/hicolor/scalable/apps");
        }
        for (const auto& D : NHyprCommon::xdgDataDirs())
            add(D + "/pixmaps");
        for (const auto& B : BASES)
            for (const char* CTX : {"devices", "status", "apps", "legacy"})
                add(B + "/Adwaita/symbolic/" + CTX);
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

    // Application dirs in XDG order: per-user first (it overrides system),
    // then the data dirs (Flatpak/system exports).
    static std::vector<std::string> appDirs() {
        auto dirs = NHyprCommon::xdgDataDirs();
        for (auto& D : dirs)
            D += "/applications";
        return dirs;
    }

    static void indexDesktopEntry(const NHyprCommon::CAsyncFileIndex::SEntry& entry) {
        std::istringstream F(entry.contents);
        std::string        icon, wmClass, line;
        bool               inEntry = false;
        while (std::getline(F, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.starts_with("[")) {
                if (inEntry)
                    break;
                inEntry = line == "[Desktop Entry]";
                continue;
            }
            if (!inEntry)
                continue;
            if (icon.empty() && line.starts_with("Icon=")) {
                if (const auto VALUE = DesktopExec::unescapeString(std::string_view{line}.substr(5)))
                    icon = *VALUE;
            } else if (wmClass.empty() && line.starts_with("StartupWMClass=")) {
                if (const auto VALUE = DesktopExec::unescapeString(std::string_view{line}.substr(15)))
                    wmClass = *VALUE;
            }
            if (!icon.empty() && !wmClass.empty())
                break;
        }
        if (icon.empty())
            return;
        const auto remember = [&](const std::string& key) {
            if (!key.empty())
                desktopIcons.try_emplace(lower(key), icon);
        };
        remember(entry.path.stem().string());
        remember(wmClass);
    }

    static void startDesktopIndex() {
        desktopIcons.clear();
        desktopScanning = true;
        NHyprCommon::CAsyncFileIndex::SRequest request;
        request.generation   = ++desktopGeneration;
        request.extensions   = {".desktop"};
        request.maxEntries   = 4096;
        request.maxVisited   = 16384;
        request.maxFileBytes = DesktopExec::MAX_DESKTOP_FILE_BYTES;
        for (const auto& directory : appDirs())
            request.roots.emplace_back(directory);
        desktopIndex.request(std::move(request));
        if (desktopPoll)
            desktopPoll->updateTimeout(std::chrono::milliseconds(2));
    }

    static void pollDesktopIndex() {
        if (!desktopScanning)
            return;
        std::vector<NHyprCommon::CAsyncFileIndex::SEntry> entries;
        const bool COMPLETE = desktopIndex.poll(desktopGeneration, entries, 8);
        const size_t BEFORE = desktopIcons.size();
        for (const auto& entry : entries)
            indexDesktopEntry(entry);
        desktopScanning = !COMPLETE;
        if (desktopIcons.size() != BEFORE) {
            appIconCache.clear();
            trayIconCache.clear();
            for (const auto& item : Tray::items)
                if (item->pixels.empty() && (!item->tex || item->tex->m_texID == 0))
                    item->dirty = true;
            barChanged();
        }
        if (desktopPoll)
            desktopPoll->updateTimeout(desktopScanning ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
    }

    // Window class -> Icon= from the app's .desktop file. A window's class
    // rarely equals its desktop-file basename (ente's class is "ente" but it
    // ships ente-desktop.desktop; qBittorrent's is "qbittorrent" under
    // org.qbittorrent.qBittorrent.desktop) — the spec's StartupWMClass field
    // is the canonical association, so scan for it when the basename misses.
    static std::string desktopIconName(const std::string& identifier) {
        const auto ID = lower(identifier);
        if (const auto it = desktopIcons.find(ID); it != desktopIcons.end())
            return it->second;

        // SNI Id is stable but not required to be a desktop-file id. Accept a
        // mapped desktop identity only at an explicit suffix boundary, so an
        // id such as app_status_icon can recover "app" without letting "app"
        // match an unrelated "application" id.
        const std::string* best = nullptr;
        size_t             bestLength = 0;
        for (const auto& [key, icon] : desktopIcons) {
            if (key.size() <= bestLength || !NHyprCommon::iconIdentityPrefixMatch(ID, key))
                continue;
            best       = &icon;
            bestLength = key.size();
        }
        return best ? *best : "";
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

    SP<ITexture> trayIcon(const std::string& name, const std::string& themePath, const std::string& id) {
        const auto KEY = name + "|" + themePath + "|" + id;
        dropStaleTint();
        if (const auto IT = trayIconCache.find(KEY); IT != trayIconCache.end())
            return IT->second;

        if (!warmGate.mayBuild()) // the texture rule: only the warm builds
            return nullptr;

        auto path = resolveIconPath(name, themePath);
        if (path.empty())
            path = resolveIconPath(desktopIconName(name), themePath);
        if (path.empty())
            path = resolveIconPath(id, themePath);
        if (path.empty())
            path = resolveIconPath(desktopIconName(id), themePath);
        SP<ITexture> tex   = path.empty() ? nullptr : loadIcon(path);
        trayIconCache[KEY] = tex;
        return tex;
    }

    // A reload may follow a GTK theme switch, and everything above was
    // resolved against the old one: the dir list is probed once at init from
    // the theme name, and the caches hold the files it found. Re-probe and
    // drop them; the next warm resolves again. (resetIconNameCache forgets
    // the memoized theme name — common/icons.hpp.)
    void iconsReload() {
        NHyprCommon::resetIconNameCache();
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
        buildIconDirs();
    }

    void iconsInit() {
        if (!g_pEventLoopManager)
            return;
        desktopPoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { pollDesktopIndex(); }, nullptr);
        g_pEventLoopManager->addTimer(desktopPoll);
        startDesktopIndex();
    }

    void iconsExit() {
        if (desktopPoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(desktopPoll);
        desktopPoll.reset();
        desktopIndex.exit();
        desktopIcons.clear();
        desktopScanning = false;
        appIconCache.clear();
        namedIconCache.clear();
        trayIconCache.clear();
        iconDirs.clear();
    }

} // namespace NHyprbar
