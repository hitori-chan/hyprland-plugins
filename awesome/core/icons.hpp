// awesome/core/icons.hpp — freedesktop icon-NAME resolution, shared by every
// module that shows themed icons (the notify module's cards, the shell's task
// chips and tray). One implementation: the GTK theme's size dirs (scalable
// first, then size proximity), then hicolor, then flat pixmaps. Inheritance
// beyond hicolor isn't followed — app icons live in hicolor in practice.
//
// Pure name -> path; rasterizing stays per module (each has its own texture
// rules and caches). Misses are cached too, so a nonexistent name never
// rescans the theme. Call refreshIconTheme() on config reload: it re-reads
// the GTK theme name, and a switch (exactly what makes the old resolutions
// wrong) forgets the paths and bumps iconThemeGeneration().
#pragma once

#include <algorithm>
#include <chrono>
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
#include <utility>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
        if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
            const uint32_t H = LE32(22); // signed: negative is top-down; INT_MIN's magnitude stays 2^31 (rejected)
            return std::pair{LE32(18), (int32_t)H < 0 ? 0u - H : H};
        }
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

    inline constexpr uintmax_t MAX_DECODE_FILE_BYTES = 32u << 20;
    inline constexpr uintmax_t MAX_SVG_FILE_BYTES    = 2u << 20;

    // The file is opened ONCE, non-blocking, and judged on that open fd: a
    // path checked and then reopened by the decoder could be swapped in
    // between (a FIFO named by a sender then hangs the compositor in the
    // decoder's open), and a FIFO already there would hang a blocking open.
    namespace detail {
        struct SImageFd {
            int       fd    = -1;
            uintmax_t bytes = 0;
            ~SImageFd() {
                if (fd >= 0)
                    ::close(fd);
            }
        };
        inline bool openImage(const std::string& path, uintmax_t maxBytes, SImageFd& out) {
            out.fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY); // follows symlinks: the target counts
            if (out.fd < 0)
                return false;
            struct stat st{};
            if (fstat(out.fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
                return false;
            out.bytes = (uintmax_t)st.st_size;
            // an SVG parses and rasters on the calling thread: theme icons
            // are a few KB, and 2 MiB bounds a stranger's
            return out.bytes <= (isSvgIconPath(path) ? std::min(maxBytes, MAX_SVG_FILE_BYTES) : maxBytes);
        }
        inline size_t readAll(int fd, uint8_t* to, size_t n) {
            size_t got = 0;
            while (got < n) {
                const ssize_t R = ::read(fd, to + got, n - got);
                if (R < 0 && errno == EINTR)
                    continue;
                if (R <= 0)
                    break;
                got += (size_t)R;
            }
            return got;
        }
    }

    namespace detail {
        // the admission on an open fd: a regular file of bounded size whose
        // header declares a bounded raster (an SVG is bounded by its size
        // and the raster it is drawn at)
        inline bool admitOpen(const std::string& path, uintmax_t maxBytes, SImageFd& F) {
            if (!openImage(path, maxBytes, F))
                return false;
            if (isSvgIconPath(path))
                return true;
            // the header: 4 KiB holds every format's but a JPEG whose SOF
            // sits past a large EXIF block — that one reads on, to 256 KiB
            std::vector<uint8_t> head(std::min<uintmax_t>(F.bytes, 4096));
            head.resize(readAll(F.fd, head.data(), head.size()));
            if (!rasterDims(head.data(), head.size()) && head.size() >= 2 && head[0] == 0xff && head[1] == 0xd8 && F.bytes > head.size()) {
                const size_t HAVE = head.size();
                head.resize(std::min<uintmax_t>(F.bytes, 256u << 10));
                head.resize(HAVE + readAll(F.fd, head.data() + HAVE, head.size() - HAVE));
            }
            return admissibleRasterBytes(head.data(), head.size());
        }
    }

    // A file the plugin may decode (any session-bus sender — a
    // notification's image-path, an SNI icon theme path — names it). For a
    // check only: a decoder takes readImageFile's bytes or
    // openAdmissibleImage's held file instead of reopening the path.
    inline bool admissibleImageFile(const std::string& path, uintmax_t maxBytes = MAX_DECODE_FILE_BYTES) {
        detail::SImageFd F;
        return detail::admitOpen(path, maxBytes, F);
    }

    // The whole admitted file, for a decoder on this thread that decodes
    // THESE bytes: judged on the same open fd they are read from.
    inline std::optional<std::vector<uint8_t>> readImageFile(const std::string& path, uintmax_t maxBytes = MAX_DECODE_FILE_BYTES) {
        detail::SImageFd F;
        if (!detail::admitOpen(path, maxBytes, F) || lseek(F.fd, 0, SEEK_SET) != 0)
            return std::nullopt;
        std::vector<uint8_t> bytes((size_t)F.bytes);
        bytes.resize(detail::readAll(F.fd, bytes.data(), bytes.size()));
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    }

    // An admitted file held open, for a decoder off this thread that takes a
    // path: it reads /proc/self/fd/N, the very inode that was checked. Close
    // it only once that decoder is done with it.
    class CImageFile {
      public:
        CImageFile() = default;
        explicit CImageFile(int fd) : m_fd(fd) {}
        CImageFile(CImageFile&& o) noexcept : m_fd(std::exchange(o.m_fd, -1)) {}
        CImageFile& operator=(CImageFile&& o) noexcept {
            if (this != &o) {
                reset();
                m_fd = std::exchange(o.m_fd, -1);
            }
            return *this;
        }
        CImageFile(const CImageFile&)            = delete;
        CImageFile& operator=(const CImageFile&) = delete;
        ~CImageFile() {
            reset();
        }
        void reset() {
            if (m_fd >= 0)
                ::close(m_fd);
            m_fd = -1;
        }
        // hand the fd over without closing it (a decoder still reading)
        int release() {
            return std::exchange(m_fd, -1);
        }
        std::string path() const {
            return "/proc/self/fd/" + std::to_string(m_fd);
        }
        explicit operator bool() const {
            return m_fd >= 0;
        }

      private:
        int m_fd = -1;
    };
    inline CImageFile openAdmissibleImage(const std::string& path, uintmax_t maxBytes = MAX_DECODE_FILE_BYTES) {
        detail::SImageFd F;
        if (!detail::admitOpen(path, maxBytes, F))
            return {};
        return CImageFile{std::exchange(F.fd, -1)};
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

    // The name memo, two generations: a hit in the old one moves to the
    // young one, and when the young one fills, the old one goes. Recent
    // names survive the bound (a plain clear at the bound rescanned every
    // live app's icon), and the memory stays two generations' worth.
    struct SIconNameMemo {
        std::unordered_map<std::string, std::string> young, old;
        void                                         clear() {
            young.clear();
            old.clear();
        }
    };
    inline SIconNameMemo& iconNameCache() {
        static SIconNameMemo C;
        return C;
    }

    // An uncached resolve walks the theme tree: ~650 stats for a name the
    // GTK theme has, ~2000 for one nobody has (~3 ms) — on the compositor
    // thread, and any session-bus sender names them (a Notify's app_icon
    // and four more hints, an SNI IconName). Past this many walks a window
    // the rest resolve to nothing, uncached, so they are tried again later:
    // a burst of distinct names costs a bounded stall, never one per name.
    // A short window (32 a second on average) keeps one burst to ~25 ms,
    // where 32 back to back were ~100 ms in one loop turn.
    inline constexpr int  MAX_ICON_WALKS_PER_WINDOW = 8;
    inline constexpr auto ICON_WALK_WINDOW          = std::chrono::milliseconds(250);
    // Set by a resolve the budget refused: the name is unresolved FOR NOW,
    // not missing. A caller that memoizes misses clears it before a lookup
    // and must not memoize a deferral (it would stay blank until a reload).
    inline bool& iconDeferred() {
        static bool B = false;
        return B;
    }
    inline bool iconWalkBudget() {
        static auto WINDOW = std::chrono::steady_clock::now();
        static int  walks  = 0;
        const auto  NOW    = std::chrono::steady_clock::now();
        if (NOW - WINDOW >= ICON_WALK_WINDOW) {
            WINDOW = NOW;
            walks  = 0;
        }
        if (walks < MAX_ICON_WALKS_PER_WINDOW) {
            walks++;
            return true;
        }
        iconDeferred() = true;
        return false;
    }

    struct SThemeName {
        std::string value;
        bool        read = false;
    };
    inline SThemeName& iconThemeName() {
        static SThemeName T;
        return T;
    }

    inline uint64_t& iconThemeGeneration() {
        static uint64_t G = 0;
        return G;
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

    // A config reload may follow a theme switch. Re-read the theme name;
    // only a switch forgets the memo and bumps the generation, which each
    // module compares to the one it last saw (several modules refresh per
    // reload: the first one to see a switch must not hide it from the rest).
    // Same theme: only the misses go, so icons installed since resolve.
    inline void refreshIconTheme() {
        auto&      T    = iconThemeName();
        const auto WAS  = T;
        T               = {};
        const auto NOW  = gtkIconThemeName();
        auto&      MEMO = iconNameCache();
        if (WAS.read && WAS.value == NOW) {
            std::erase_if(MEMO.young, [](const auto& E) { return E.second.empty(); });
            std::erase_if(MEMO.old, [](const auto& E) { return E.second.empty(); });
            return;
        }
        MEMO.clear();
        ++iconThemeGeneration();
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
        if (const auto IT = CACHE.young.find(KEY); IT != CACHE.young.end())
            return IT->second;
        if (const auto IT = CACHE.old.find(KEY); IT != CACHE.old.end()) {
            auto found = std::move(IT->second);
            CACHE.old.erase(IT);
            return CACHE.young.emplace(KEY, std::move(found)).first->second;
        }
        if (!iconWalkBudget())
            return "";
        if (CACHE.young.size() >= MAX_ICON_NAME_CACHE / 2) {
            CACHE.old = std::move(CACHE.young);
            CACHE.young.clear();
        }

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

        CACHE.young[KEY] = found;
        return found;
    }

} // namespace NAwesome
