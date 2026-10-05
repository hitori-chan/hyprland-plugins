// awesome/core/icons.hpp — freedesktop icon-NAME resolution, shared by every
// module that shows themed icons (the notify module's cards, the shell's task
// chips and tray). One implementation: the GTK theme's size dirs (scalable
// first, then size proximity), then hicolor, then flat pixmaps. Inheritance
// beyond hicolor isn't followed — app icons live in hicolor in practice.
//
// Pure name -> path; rasterizing stays per module (each has its own texture
// rules and caches). Misses are cached too, so a nonexistent name never
// rescans the theme. Call resetIconNameCache() on config reload — it forgets
// the memoized GTK theme name along with the paths, since a theme switch is
// exactly what makes the old resolutions wrong.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NAwesome {

    inline bool iconIdentityPrefixMatch(std::string_view identifier, std::string_view candidate) {
        if (candidate.size() < 3 || candidate.size() >= identifier.size() || !identifier.starts_with(candidate))
            return false;
        const char boundary = identifier[candidate.size()];
        return boundary == '_' || boundary == '-';
    }

    inline bool isSvgIconPath(std::string_view source) {
        if (source.size() < 4)
            return false;
        const auto ext = source.substr(source.size() - 4);
        const auto eq  = [](char value, char lower) { return value == lower || value == lower - ('a' - 'A'); };
        return ext[0] == '.' && eq(ext[1], 's') && eq(ext[2], 'v') && eq(ext[3], 'g');
    }

    // The pixel size a raster declares in its header, before anything
    // decodes it: a few-KB compressed image can declare 20000x20000 (1.6 GB
    // decoded). PNG, JPEG, WebP, GIF and BMP — the formats icons and
    // notification images come in; anything else answers nullopt.
    inline std::optional<std::pair<uint32_t, uint32_t>> rasterDims(const uint8_t* d, size_t n) {
        const auto BE16 = [&](size_t o) { return o + 2 <= n ? (uint32_t)d[o] << 8 | d[o + 1] : 0u; };
        const auto BE32 = [&](size_t o) { return o + 4 <= n ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0u; };
        const auto LE16 = [&](size_t o) { return o + 2 <= n ? (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 : 0u; };
        const auto LE24 = [&](size_t o) { return o + 3 <= n ? (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 : 0u; };
        const auto LE32 = [&](size_t o) { return o + 4 <= n ? LE16(o) | LE16(o + 2) << 16 : 0u; };
        if (n >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8) && !memcmp(d + 12, "IHDR", 4))
            return std::pair{BE32(16), BE32(20)};
        if (n >= 10 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
            return std::pair{LE16(6), LE16(8)};
        if (n >= 26 && d[0] == 'B' && d[1] == 'M')
            return std::pair{LE32(18), (uint32_t)std::abs((int32_t)LE32(22))};
        if (n >= 30 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
            if (!memcmp(d + 12, "VP8X", 4))
                return std::pair{LE24(24) + 1, LE24(27) + 1};
            if (!memcmp(d + 12, "VP8L", 4) && n >= 25) {
                const uint32_t B = LE32(21);
                return std::pair{(B & 0x3fff) + 1, (B >> 14 & 0x3fff) + 1};
            }
            if (!memcmp(d + 12, "VP8 ", 4))
                return std::pair{LE16(26) & 0x3fff, LE16(28) & 0x3fff};
            return std::nullopt;
        }
        if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) { // JPEG: walk the segments to a SOFn
            size_t o = 2;
            while (o + 9 <= n) {
                if (d[o] != 0xff)
                    return std::nullopt;
                const uint8_t M = d[o + 1];
                if (M == 0xff) {
                    o++;
                    continue;
                }
                if (M >= 0xc0 && M <= 0xcf && M != 0xc4 && M != 0xc8 && M != 0xcc)
                    return std::pair{BE16(o + 7), BE16(o + 5)};
                if (M == 0xd8 || M == 0x01 || (M >= 0xd0 && M <= 0xd7)) {
                    o += 2;
                    continue;
                }
                o += 2 + BE16(o + 2);
            }
        }
        return std::nullopt;
    }

    // Every decode of client-influenced bytes stays under this many pixels.
    inline constexpr uint64_t MAX_DECODE_PIXELS = 16ull << 20;
    inline bool admissibleRasterBytes(const uint8_t* d, size_t n) {
        const auto DIMS = rasterDims(d, n);
        return DIMS && DIMS->first > 0 && DIMS->second > 0 && (uint64_t)DIMS->first * DIMS->second <= MAX_DECODE_PIXELS;
    }

    // A file the plugin may decode on the main thread: a regular file (a
    // FIFO or device path would block the compositor in open/read, and any
    // session-bus sender — a notification's image-path, an SNI icon theme
    // path — names the file) of bounded size, whose header declares a
    // bounded raster (an SVG is bounded by its size and the raster it is
    // drawn at). Every decode entry point that takes a client-influenced
    // path checks this first.
    inline constexpr uintmax_t MAX_DECODE_FILE_BYTES = 32u << 20;
    inline constexpr uintmax_t MAX_SVG_FILE_BYTES    = 2u << 20;
    inline bool admissibleImageFile(const std::string& path, uintmax_t maxBytes = MAX_DECODE_FILE_BYTES) {
        std::error_code ec;
        const auto      STATUS = std::filesystem::status(path, ec); // follows symlinks: the target counts
        if (ec || !std::filesystem::is_regular_file(STATUS))
            return false;
        const auto BYTES = std::filesystem::file_size(path, ec);
        if (ec || BYTES > maxBytes)
            return false;
        // an SVG parses and rasters on the calling thread: theme icons are
        // a few KB, and 2 MiB bounds a stranger's
        if (isSvgIconPath(path))
            return BYTES <= MAX_SVG_FILE_BYTES;
        // the header: 4 KiB holds every format's but a JPEG whose SOF sits
        // past a large EXIF block — that one reads on, to 256 KiB
        std::ifstream        F(path, std::ios::binary);
        std::vector<uint8_t> head(std::min<uintmax_t>(BYTES, 4096));
        if (!F.read((char*)head.data(), (std::streamsize)head.size()))
            return false;
        if (!rasterDims(head.data(), head.size()) && head.size() >= 2 && head[0] == 0xff && head[1] == 0xd8 && BYTES > head.size()) {
            const size_t HAVE = head.size();
            head.resize(std::min<uintmax_t>(BYTES, 256u << 10));
            if (!F.read((char*)head.data() + HAVE, (std::streamsize)(head.size() - HAVE)))
                return false;
        }
        return admissibleRasterBytes(head.data(), head.size());
    }

    // The XDG data dirs in precedence order: the per-user one first (it
    // overrides), then $XDG_DATA_DIRS. Every freedesktop lookup a plugin
    // does — icon themes, .desktop entries, pixmaps — walks this list.
    inline std::vector<std::string> xdgDataDirs() {
        std::vector<std::string> dirs;
        if (const char* X = getenv("XDG_DATA_HOME"); X && *X)
            dirs.push_back(X);
        else if (const char* H = getenv("HOME"); H && *H)
            dirs.push_back(std::string(H) + "/.local/share");
        std::string data = "/usr/local/share:/usr/share";
        if (const char* X = getenv("XDG_DATA_DIRS"); X && *X)
            data = X;
        for (size_t p = 0; p < data.size();) {
            const auto E = data.find(':', p);
            auto       D = data.substr(p, E == std::string::npos ? E : E - p);
            while (D.size() > 1 && D.back() == '/')
                D.pop_back();
            if (!D.empty())
                dirs.push_back(std::move(D));
            if (E == std::string::npos)
                break;
            p = E + 1;
        }
        return dirs;
    }

    // Where icon THEMES live: the data dirs' icons/ plus the legacy ~/.icons
    // (second, right after the per-user data dir — the spec's order).
    inline std::vector<std::string> xdgIconBases() {
        const auto               DATA = xdgDataDirs();
        std::vector<std::string> bases;
        bases.reserve(DATA.size() + 1);
        if (!DATA.empty())
            bases.push_back(DATA.front() + "/icons");
        if (const char* H = getenv("HOME"); H && *H)
            bases.push_back(std::string(H) + "/.icons");
        for (size_t i = 1; i < DATA.size(); i++)
            bases.push_back(DATA[i] + "/icons");
        return bases;
    }

    inline std::unordered_map<std::string, std::string>& iconNameCache() {
        static std::unordered_map<std::string, std::string> C;
        return C;
    }

    struct SThemeName {
        std::string value;
        bool        read = false;
    };
    inline SThemeName& iconThemeName() {
        static SThemeName T;
        return T;
    }

    inline void resetIconNameCache() {
        iconNameCache().clear();
        iconThemeName() = {};
    }

    // The GTK icon theme is this system's source of truth (Qt follows it). Read
    // gtk-icon-theme-name from settings.ini once; fall back to hicolor.
    inline std::string gtkIconThemeName() {
        auto& T = iconThemeName();
        if (T.read)
            return T.value;
        T.read = true;

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
                        T.value = line.substr(EQ + 1);
                        T.value.erase(0, T.value.find_first_not_of(" \t"));
                        T.value.erase(T.value.find_last_not_of(" \t\r\n") + 1);
                    }
                    break;
                }
        }
        return T.value;
    }

    // Look for <dir>/name.ext directly and one category level down
    // (<dir>/<cat>/name.ext) — the freedesktop size dirs hold either.
    inline std::string findIconInDir(const std::string& dir, const std::string& name) {
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

    // Resolve a freedesktop icon NAME to a file path via themed lookup; "" if
    // unresolved or if the string is already a path. Cached per name AND
    // size: the requested size leads sizeDirs below, so in a PNG-only theme
    // it CHOOSES the file — keyed on the name alone, whichever caller asked
    // first pinned the size for every later one (the notify module wants a card
    // icon at max_icon and an action icon at ~15px).
    // Names come from session-bus senders (a notification's app_icon, an
    // SNI IconName): only a plausible freedesktop icon name is worth a theme
    // scan, and the memo is bounded (cleared at the bound: a re-resolution
    // is a few stats, an unbounded map is a leak a sender controls).
    inline constexpr size_t MAX_ICON_NAME_CACHE = 2048;
    inline bool plausibleIconName(const std::string& name) {
        if (name.empty() || name.size() > 128)
            return false;
        return std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+' || c == '@'; });
    }

    inline std::string resolveIconName(const std::string& name, int sizePx) {
        if (!plausibleIconName(name))
            return ""; // a path, nothing, or not an icon name
        auto&      CACHE = iconNameCache();
        const auto KEY   = name + "\x1f" + std::to_string(sizePx);
        if (const auto IT = CACHE.find(KEY); IT != CACHE.end())
            return IT->second;
        if (CACHE.size() >= MAX_ICON_NAME_CACHE)
            CACHE.clear();

        const auto               bases = xdgIconBases();

        std::vector<std::string> themes;
        if (const auto GT = gtkIconThemeName(); !GT.empty()) {
            themes.push_back(GT);
            // a "-dark"/"-light" variant usually inherits its base — a cheap
            // approximation of index.theme inheritance
            for (const char* SUF : {"-dark", "-light", "-Dark", "-Light"})
                if (GT.ends_with(SUF))
                    themes.push_back(GT.substr(0, GT.size() - std::string(SUF).size()));
        }
        themes.push_back("hicolor");
        themes.push_back("Adwaita"); // the freedesktop-name last resorts
        themes.push_back("AdwaitaLegacy");

        // Adwaita stores symbolic marks as symbolic/<context>/name.svg,
        // unlike themes that use scalable/<context>/ or size/<context>/.
        // Treat symbolic as a size directory so findIconInDir also probes the
        // context below it.
        std::vector<std::string> sizeDirs = {"scalable", "symbolic"};
        // Small sizes in proximity order, the large ones LAST: an app that
        // ships a single large icon (discord ships 256x256 only) must still
        // resolve, but when both exist a source near the request wins. 22 is
        // load-bearing, not a nicety: nm-applet's whole nm-* notification
        // set (nm-signal-*, nm-stage*, nm-tech-*) is 22x22 only.
        for (const int S : {sizePx, 64, 48, 96, 128, 256, 72, 36, 32, 24, 22, 20, 16, 192, 384, 512, 1024})
            sizeDirs.push_back(std::to_string(S) + "x" + std::to_string(S));

        // breeze (and KDE themes generally) lay out <context>/<size> instead
        // of <size>x<size>/<context> — probe the common contexts too
        static const char*       CTXS[]   = {"status", "apps", "devices", "actions", "categories", "mimetypes", "legacy", "symbolic"};
        std::vector<std::string> ctxSizes = {"symbolic", "scalable"};
        for (const int S : {sizePx, 64, 48, 32, 24, 22, 20, 16, 96, 128, 256, 512})
            ctxSizes.push_back(std::to_string(S));

        std::string found;
        for (const auto& THEME : themes) {
            for (const auto& BASE : bases) {
                const auto      TDIR = BASE + "/" + THEME;
                std::error_code ec;
                if (!std::filesystem::is_directory(TDIR, ec))
                    continue;
                for (const auto& SD : sizeDirs)
                    if (found = findIconInDir(TDIR + "/" + SD, name); !found.empty())
                        break;
                for (const char* CTX : CTXS) {
                    if (!found.empty())
                        break;
                    if (!std::filesystem::is_directory(TDIR + "/" + CTX, ec))
                        continue;
                    for (const auto& SZ : ctxSizes) {
                        static const char* EXT[] = {".svg", ".png"};
                        for (const char* E : EXT)
                            if (std::filesystem::exists(TDIR + "/" + CTX + "/" + SZ + "/" + name + E, ec)) {
                                found = TDIR + "/" + CTX + "/" + SZ + "/" + name + E;
                                break;
                            }
                        if (!found.empty())
                            break;
                    }
                }
                if (!found.empty())
                    break;
            }
            if (!found.empty())
                break;
        }
        if (found.empty())
            for (const auto& D : xdgDataDirs()) { // flat pixmaps, the pre-theme layout
                std::error_code ec;
                for (const char* e : {".svg", ".png", ".xpm"})
                    if (const auto P = D + "/pixmaps/" + name + e; std::filesystem::exists(P, ec)) {
                        found = P;
                        break;
                    }
                if (!found.empty())
                    break;
            }

        CACHE[KEY] = found;
        return found;
    }

} // namespace NAwesome
