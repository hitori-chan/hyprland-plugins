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
#include <memory>
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

    // A theme dir without a usable icon cache is walked: ~650 stats for a
    // name the theme has, ~2000 for one nobody has — on the compositor
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

    // the misses go (an icon installed since resolves); hits stay
    inline void dropIconMisses() {
        auto& MEMO = iconNameCache();
        std::erase_if(MEMO.young, [](const auto& E) { return E.second.empty(); });
        std::erase_if(MEMO.old, [](const auto& E) { return E.second.empty(); });
    }

    // GTK's index of a theme dir, icon-theme.cache (gtk-update-icon-cache;
    // the package manager's hook rewrites every packaged theme's): a name is
    // a hash bucket and a few short reads, where the walk is hundreds of
    // stats and a miss thousands — 35 ms warm, 180 ms cold for a name breeze
    // and hicolor both lack, on the compositor thread. Trusted as GTK trusts
    // it: only when no older than its dir. Read with pread on a held fd,
    // never mapped (a cache truncated under a mapping would SIGBUS the
    // compositor), every offset checked against the size at open: a corrupt
    // one answers nothing, never a read out of bounds or an endless chain.
    class CIconThemeCache {
      public:
        static constexpr uint16_t HAS_XPM = 1, HAS_SVG = 2, HAS_PNG = 4;

        CIconThemeCache()                                  = default;
        CIconThemeCache(const CIconThemeCache&)            = delete;
        CIconThemeCache& operator=(const CIconThemeCache&) = delete;
        ~CIconThemeCache() {
            if (m_fd >= 0)
                ::close(m_fd);
        }

        // false: no cache, a stale one, or one that does not parse
        bool open(const std::string& themeDir) {
            struct stat dir{}, st{};
            if (::stat(themeDir.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode))
                return false;
            m_fd = ::open((themeDir + "/icon-theme.cache").c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
            if (m_fd < 0 || fstat(m_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12 || (uintmax_t)st.st_size > MAX_BYTES || st.st_mtime < dir.st_mtime)
                return false;
            m_size           = (size_t)st.st_size;
            uint32_t version = 0, dirList = 0, nDirs = 0;
            // version 1.0 only, as GTK reads it
            if (!u32(0, version) || version != 0x00010000 || !u32(4, m_hash) || !u32(8, dirList) || !u32(m_hash, m_buckets) || m_buckets == 0 ||
                m_buckets > (m_size - m_hash - 4) / 4 || !u32(dirList, nDirs) || nDirs > MAX_DIRS)
                return false;
            m_dirs.reserve(nDirs);
            for (uint32_t i = 0; i < nDirs; i++) {
                uint32_t off = 0;
                if (!u32(dirList + 4ull + 4ull * i, off))
                    return false;
                m_dirs.push_back(str(off));
            }
            return true;
        }

        // every image the cache lists for name: its dir under the theme and
        // which of .svg/.png/.xpm it has there
        template <class FN>
        void images(const std::string& name, FN&& each) const {
            if (m_fd < 0 || m_buckets == 0 || name.empty() || name.size() > MAX_NAME)
                return;
            uint32_t chain = 0;
            if (!u32(m_hash + 4ull + 4ull * (hash(name) % m_buckets), chain))
                return;
            char got[MAX_NAME + 1];
            for (int hop = 0; chain != 0xffffffffu && hop < MAX_CHAIN; hop++) {
                uint32_t next = 0, nameAt = 0, list = 0;
                if (!u32(chain, next) || !u32(chain + 4ull, nameAt) || !u32(chain + 8ull, list))
                    return;
                if (read(nameAt, got, name.size() + 1) && got[name.size()] == '\0' && !memcmp(got, name.data(), name.size())) {
                    uint32_t n = 0;
                    if (!u32(list, n))
                        return;
                    for (uint32_t i = 0; i < std::min(n, MAX_IMAGES); i++) {
                        uint8_t im[4];
                        if (!read(list + 4ull + 8ull * i, im, sizeof im))
                            return;
                        const uint16_t DIR = im[0] << 8 | im[1], FLAGS = im[2] << 8 | im[3];
                        if (DIR < m_dirs.size() && !m_dirs[DIR].empty())
                            each(m_dirs[DIR], FLAGS);
                    }
                    return;
                }
                chain = next;
            }
        }

      private:
        static constexpr uintmax_t MAX_BYTES  = 64u << 20;
        static constexpr uint32_t  MAX_DIRS   = 4096;
        static constexpr int       MAX_CHAIN  = 4096; // a corrupt chain's cycle ends here
        static constexpr uint32_t  MAX_IMAGES = 1024;
        static constexpr size_t    MAX_NAME   = 128; // plausibleIconName's bound

        // GTK's icon_name_hash, over signed chars
        static uint32_t hash(const std::string& s) {
            uint32_t h = (uint32_t)(signed char)s[0];
            for (size_t i = 1; i < s.size(); i++)
                h = (h << 5) - h + (uint32_t)(signed char)s[i];
            return h;
        }
        bool read(uint64_t off, void* to, size_t n) const {
            if (off + n > m_size)
                return false;
            for (size_t got = 0; got < n;) {
                const ssize_t R = ::pread(m_fd, (char*)to + got, n - got, (off_t)(off + got));
                if (R < 0 && errno == EINTR)
                    continue;
                if (R <= 0)
                    return false; // truncated since open
                got += (size_t)R;
            }
            return true;
        }
        bool u32(uint64_t off, uint32_t& v) const {
            uint8_t b[4];
            if (!read(off, b, sizeof b))
                return false;
            v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            return true;
        }
        // a dir name, NUL-terminated within 256 bytes; else "" (never matches)
        std::string str(uint64_t off) const {
            char         buf[256];
            const size_t N = off < m_size ? (size_t)std::min<uint64_t>(sizeof buf, m_size - off) : 0;
            if (!N || !read(off, buf, N))
                return "";
            const auto* END = (const char*)memchr(buf, '\0', N);
            return END ? std::string(buf, (size_t)(END - buf)) : "";
        }

        int                      m_fd   = -1;
        size_t                   m_size = 0;
        uint32_t                 m_hash = 0, m_buckets = 0;
        std::vector<std::string> m_dirs;
    };

    // Each theme dir the chain names, as last probed: absent, indexed by a
    // fresh cache, or walked. Re-probed at most every ICON_DIR_PROBE (an
    // install rewrites hicolor's cache: a new inode, a new mtime); a change
    // drops the memo's misses, so what the new cache holds resolves.
    struct SIconDirSig {
        bool    dir = false, cache = false;
        dev_t   dev    = 0;
        ino_t   dirIno = 0, ino = 0;
        off_t   size     = 0;
        int64_t dirMtime = 0, mtime = 0;
        bool    operator==(const SIconDirSig&) const = default;
    };
    struct SIconThemeDir {
        enum eKind : uint8_t {
            ABSENT,
            CACHED,
            WALKED,
        } kind = ABSENT;
        std::unique_ptr<CIconThemeCache>      cache;
        SIconDirSig                           sig;
        std::chrono::steady_clock::time_point probed;
    };
    inline constexpr auto                                  ICON_DIR_PROBE      = std::chrono::seconds(5);
    inline constexpr size_t                                MAX_ICON_THEME_DIRS = 256; // themes x bases; the env names the bases
    inline std::unordered_map<std::string, SIconThemeDir>& iconThemeDirs() {
        static std::unordered_map<std::string, SIconThemeDir> D;
        return D;
    }
    inline SIconDirSig iconDirSig(const std::string& themeDir) {
        SIconDirSig S;
        struct stat st{};
        const auto  NS = [](const struct timespec& t) { return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; };
        if (::stat(themeDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return S;
        S.dir      = true;
        S.dirIno   = st.st_ino;
        S.dirMtime = NS(st.st_mtim);
        if (::stat((themeDir + "/icon-theme.cache").c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            S.cache = true;
            S.dev   = st.st_dev;
            S.ino   = st.st_ino;
            S.size  = st.st_size;
            S.mtime = NS(st.st_mtim);
        }
        return S;
    }
    inline const SIconThemeDir& iconThemeDir(const std::string& themeDir) {
        auto&      DIRS = iconThemeDirs();
        const auto NOW  = std::chrono::steady_clock::now();
        auto       IT   = DIRS.find(themeDir);
        if (IT != DIRS.end() && NOW - IT->second.probed < ICON_DIR_PROBE)
            return IT->second;
        const auto SIG = iconDirSig(themeDir);
        if (IT != DIRS.end() && IT->second.sig == SIG) {
            IT->second.probed = NOW;
            return IT->second;
        }
        if (IT == DIRS.end()) {
            if (DIRS.size() >= MAX_ICON_THEME_DIRS)
                DIRS.clear();
            IT = DIRS.emplace(themeDir, SIconThemeDir{}).first;
        } else
            dropIconMisses();
        auto& D  = IT->second;
        D        = SIconThemeDir{};
        D.sig    = SIG;
        D.probed = NOW;
        if (SIG.dir) {
            auto C = std::make_unique<CIconThemeCache>();
            D.kind = C->open(themeDir) ? SIconThemeDir::CACHED : SIconThemeDir::WALKED;
            if (D.kind == SIconThemeDir::CACHED)
                D.cache = std::move(C);
        }
        return D;
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
        iconThemeDirs().clear(); // re-probed on the next resolve
        if (WAS.read && WAS.value == NOW) {
            dropIconMisses();
            return;
        }
        MEMO.clear();
        ++iconThemeGeneration();
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

    // The order a theme dir is searched in, the walk's and the cache's
    // alike. sizeDirs: <size>/[<context>/]name, hicolor's layout. Small
    // sizes in proximity order, the large ones LAST: an app that ships a
    // single large icon (discord ships 256x256 only) must still resolve, but
    // when both exist a source near the request wins. 22 is load-bearing,
    // not a nicety: nm-applet's whole nm-* notification set (nm-signal-*,
    // nm-stage*, nm-tech-*) is 22x22 only. Adwaita stores symbolic marks as
    // symbolic/<context>/name.svg: symbolic counts as a size dir. ctxSizes:
    // <context>/<size>/name, breeze's (KDE's) layout, after every size dir.
    inline constexpr const char* ICON_CONTEXTS[] = {"status", "apps", "devices", "actions", "categories", "mimetypes", "legacy", "symbolic"};
    struct SIconSearch {
        std::vector<std::string> sizeDirs, ctxSizes;
    };
    inline SIconSearch iconSearch(int sizePx) {
        SIconSearch S{.sizeDirs = {"scalable", "symbolic"}, .ctxSizes = {"symbolic", "scalable"}};
        for (const int SZ : {sizePx, 64, 48, 96, 128, 256, 72, 36, 32, 24, 22, 20, 16, 192, 384, 512, 1024})
            S.sizeDirs.push_back(std::to_string(SZ) + "x" + std::to_string(SZ));
        for (const int SZ : {sizePx, 64, 48, 32, 24, 22, 20, 16, 96, 128, 256, 512})
            S.ctxSizes.push_back(std::to_string(SZ));
        return S;
    }

    inline std::string walkThemeDir(const std::string& themeDir, const std::string& name, const SIconSearch& S) {
        for (const auto& SD : S.sizeDirs)
            if (auto found = findIconInDir(themeDir + "/" + SD, name); !found.empty())
                return found;
        std::error_code ec;
        for (const char* CTX : ICON_CONTEXTS) {
            if (!std::filesystem::is_directory(themeDir + "/" + CTX, ec))
                continue;
            for (const auto& SZ : S.ctxSizes)
                for (const char* E : {".svg", ".png"})
                    if (auto P = themeDir + "/" + CTX + "/" + SZ + "/" + name + E; std::filesystem::exists(P, ec))
                        return P;
        }
        return "";
    }

    // where the walk finds a file in this dir, as a rank (lower first): a
    // size dir before the context layout, a file directly in a size dir
    // before one a context down. nullopt: a dir the walk never looks in.
    inline std::optional<size_t> iconDirRank(std::string_view dir, const SIconSearch& S) {
        const auto SLASH = dir.find('/');
        const auto HEAD  = dir.substr(0, SLASH);
        const auto TAIL  = SLASH == std::string_view::npos ? std::string_view{} : dir.substr(SLASH + 1);
        if (TAIL.find('/') != std::string_view::npos)
            return std::nullopt;
        if (const auto IT = std::ranges::find(S.sizeDirs, HEAD); IT != S.sizeDirs.end())
            return (size_t)(IT - S.sizeDirs.begin()) * 2 + (TAIL.empty() ? 0 : 1);
        if (TAIL.empty())
            return std::nullopt;
        const auto CTX = std::ranges::find(ICON_CONTEXTS, HEAD);
        const auto SZ  = std::ranges::find(S.ctxSizes, TAIL);
        if (CTX == std::end(ICON_CONTEXTS) || SZ == S.ctxSizes.end())
            return std::nullopt;
        return S.sizeDirs.size() * 2 + (size_t)(CTX - std::begin(ICON_CONTEXTS)) * S.ctxSizes.size() + (size_t)(SZ - S.ctxSizes.begin());
    }

    // name in one theme dir, into found: from its cache when it has a fresh
    // one (the best-ranked listed file still there; .svg before .png before
    // .xpm, as the walk probes), else by the walk, which the budget meters
    // once per resolve. false: the budget refused the walk.
    inline bool findInThemeDir(const std::string& themeDir, const std::string& name, const SIconSearch& S, bool& walked, std::string& found) {
        const auto& D = iconThemeDir(themeDir);
        if (D.kind == SIconThemeDir::ABSENT)
            return true;
        if (D.kind == SIconThemeDir::CACHED) {
            std::vector<std::pair<size_t, std::string>> hits;
            D.cache->images(name, [&](const std::string& dir, uint16_t flags) {
                const auto RANK = iconDirRank(dir, S);
                if (!RANK)
                    return;
                const bool CONTEXT_LAYOUT = *RANK >= S.sizeDirs.size() * 2;
                size_t     e              = 0;
                for (const auto& [FLAG, EXT] :
                     {std::pair{CIconThemeCache::HAS_SVG, ".svg"}, std::pair{CIconThemeCache::HAS_PNG, ".png"}, std::pair{CIconThemeCache::HAS_XPM, ".xpm"}}) {
                    if ((flags & FLAG) && !(CONTEXT_LAYOUT && FLAG == CIconThemeCache::HAS_XPM))
                        hits.emplace_back(*RANK * 4 + e, themeDir + "/" + dir + "/" + name + EXT);
                    e++;
                }
            });
            std::ranges::sort(hits);
            std::error_code ec;
            for (auto& [RANK, P] : hits)
                if (std::filesystem::exists(P, ec)) {
                    found = std::move(P);
                    break;
                }
            return true;
        }
        if (!walked) {
            if (!iconWalkBudget())
                return false;
            walked = true;
        }
        found = walkThemeDir(themeDir, name, S);
        return true;
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

        const auto  BASES  = xdgIconBases();
        const auto  SEARCH = iconSearch(sizePx);
        std::string found;
        bool        walked = false;
        for (const auto& THEME : themes) {
            for (const auto& BASE : BASES)
                if (!findInThemeDir(BASE + "/" + THEME, name, SEARCH, walked, found))
                    return ""; // deferred (iconDeferred), not memoized
                else if (!found.empty())
                    break;
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

        if (CACHE.young.size() >= MAX_ICON_NAME_CACHE / 2) {
            CACHE.old = std::move(CACHE.young);
            CACHE.young.clear();
        }
        CACHE.young[KEY] = found;
        return found;
    }

} // namespace NAwesome
