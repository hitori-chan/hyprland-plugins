#include "theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace VClick::Theme {
    constexpr uint32_t MAGIC      = 0x72756358; // "Xcur", little-endian
    constexpr uint32_t IMAGE_TYPE = 0xfffd0002;
    constexpr size_t   CHUNK      = 36; // an image chunk's header: 4 common fields, then width, height, xhot, yhot, delay

    static uint32_t    le32(const unsigned char* p) {
        return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
    }

    std::expected<SImage, std::string> readFile(const std::string& path, int size) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return std::unexpected("cannot open " + path);
        const auto READ = [&](uint64_t at, unsigned char* dst, size_t n) {
            f.clear();
            f.seekg(static_cast<std::streamoff>(at));
            f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
            return f.gcount() == static_cast<std::streamsize>(n);
        };

        unsigned char head[16];
        if (!READ(0, head, sizeof head) || le32(head) != MAGIC)
            return std::unexpected("not an XCursor file");
        const uint32_t HEADER = le32(head + 4), NTOC = le32(head + 12);
        if (HEADER < sizeof head || NTOC == 0 || NTOC > MAX_TOC)
            return std::unexpected("a bad table of contents");
        std::vector<unsigned char> toc(size_t{NTOC} * 12);
        if (!READ(HEADER, toc.data(), toc.size()))
            return std::unexpected("a truncated table of contents");

        // the nominal size nearest the one wanted, the larger on a tie (as
        // a scaled output wants it sharp)
        int best = 0;
        for (uint32_t i = 0; i < NTOC; ++i) {
            const auto* E = &toc[i * 12];
            const int   S = static_cast<int>(std::min<uint32_t>(le32(E + 4), 0x7fff));
            if (le32(E) != IMAGE_TYPE || S == 0)
                continue;
            if (!best || std::abs(S - size) < std::abs(best - size) || (std::abs(S - size) == std::abs(best - size) && S > best))
                best = S;
        }
        if (!best)
            return std::unexpected("no image in " + path);

        SImage img{.nominal = best};
        for (uint32_t i = 0; i < NTOC && img.frames.size() < MAX_FRAMES; ++i) {
            const auto* E = &toc[i * 12];
            if (le32(E) != IMAGE_TYPE || static_cast<int>(le32(E + 4)) != best)
                continue;
            const uint64_t AT = le32(E + 8);
            unsigned char  c[CHUNK];
            if (!READ(AT, c, sizeof c))
                return std::unexpected("a truncated image in " + path);
            const uint32_t W = le32(c + 16), H = le32(c + 20);
            if (le32(c + 4) != IMAGE_TYPE || W == 0 || H == 0 || W > MAX_SIDE || H > MAX_SIDE)
                return std::unexpected("a bad image in " + path);
            SFrame                     fr{.w       = static_cast<int>(W),
                                          .h       = static_cast<int>(H),
                                          .hotX    = static_cast<int>(std::min(le32(c + 24), W - 1)),
                                          .hotY    = static_cast<int>(std::min(le32(c + 28), H - 1)),
                                          .delayMs = le32(c + 32)};
            std::vector<unsigned char> px(size_t{W} * H * 4);
            if (!READ(AT + CHUNK, px.data(), px.size()))
                return std::unexpected("a truncated image in " + path);
            fr.argb.resize(size_t{W} * H);
            for (size_t p = 0; p < fr.argb.size(); ++p)
                fr.argb[p] = le32(&px[p * 4]);
            img.frames.emplace_back(std::move(fr));
        }
        return img;
    }

    static std::string home(std::string_view sub) {
        const char* h = std::getenv("HOME");
        return h && *h ? std::string{h} + std::string{sub} : std::string{};
    }

    static std::vector<std::string> split(std::string_view s, std::string_view seps) {
        std::vector<std::string> out;
        size_t                   i = 0;
        while (i <= s.size()) {
            const size_t END = std::min(s.find_first_of(seps, i), s.size());
            auto         w   = s.substr(i, END - i);
            while (!w.empty() && (w.front() == ' ' || w.front() == '\t'))
                w.remove_prefix(1);
            while (!w.empty() && (w.back() == ' ' || w.back() == '\t' || w.back() == '\r'))
                w.remove_suffix(1);
            if (!w.empty())
                out.emplace_back(w);
            i = END + 1;
        }
        return out;
    }

    std::vector<std::string> searchPath() {
        std::vector<std::string> dirs;
        if (const char* p = std::getenv("XCURSOR_PATH"); p && *p) {
            for (auto& d : split(p, ":"))
                dirs.emplace_back(d.starts_with('~') ? home(std::string_view{d}.substr(1)) : d);
            return dirs;
        }
        const char* dataHome = std::getenv("XDG_DATA_HOME");
        dirs.emplace_back(dataHome && *dataHome ? std::string{dataHome} + "/icons" : home("/.local/share/icons"));
        dirs.emplace_back(home("/.icons"));
        const char* dataDirs = std::getenv("XDG_DATA_DIRS");
        for (const auto& d : split(dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share", ":"))
            dirs.emplace_back(d + "/icons");
        dirs.emplace_back("/usr/share/pixmaps");
        std::erase_if(dirs, [](const std::string& d) { return d.empty() || d.front() != '/'; });
        return dirs;
    }

    // older X names for a shape, for a theme that predates the CSS names
    static std::vector<std::string_view> namesOf(std::string_view shape) {
        static constexpr std::pair<std::string_view, std::string_view> OLDER[] = {
            {"default", "left_ptr"},
            {"context-menu", "left_ptr"},
            {"pointer", "hand2"},
            {"pointer", "hand1"},
            {"progress", "left_ptr_watch"},
            {"wait", "watch"},
            {"help", "question_arrow"},
            {"crosshair", "cross"},
            {"text", "xterm"},
            {"vertical-text", "xterm"},
            {"move", "fleur"},
            {"all-scroll", "fleur"},
            {"not-allowed", "crossed_circle"},
            {"no-drop", "crossed_circle"},
            {"grab", "openhand"},
            {"grab", "hand1"},
            {"grabbing", "closedhand"},
            {"grabbing", "fleur"},
            {"col-resize", "sb_h_double_arrow"},
            {"ew-resize", "sb_h_double_arrow"},
            {"row-resize", "sb_v_double_arrow"},
            {"ns-resize", "sb_v_double_arrow"},
            {"nesw-resize", "fd_double_arrow"},
            {"nwse-resize", "bd_double_arrow"},
            {"n-resize", "top_side"},
            {"s-resize", "bottom_side"},
            {"e-resize", "right_side"},
            {"w-resize", "left_side"},
            {"ne-resize", "top_right_corner"},
            {"nw-resize", "top_left_corner"},
            {"se-resize", "bottom_right_corner"},
            {"sw-resize", "bottom_left_corner"},
        };
        std::vector<std::string_view> out{shape};
        for (const auto& [css, x] : OLDER)
            if (css == shape)
                out.emplace_back(x);
        return out;
    }

    // a theme or shape name is one path component
    static bool plain(std::string_view s) {
        return !s.empty() && s != "." && s != ".." && !s.contains('/');
    }

    static std::vector<std::string> inheritsOf(const std::string& dir, const std::string& theme) {
        std::ifstream f(dir + "/" + theme + "/index.theme");
        std::string   line;
        while (std::getline(f, line)) {
            std::string_view l{line};
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t'))
                l.remove_prefix(1);
            if (!l.starts_with("Inherits"))
                continue;
            l.remove_prefix(8);
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t'))
                l.remove_prefix(1);
            if (l.starts_with('='))
                return split(l.substr(1), ",;");
        }
        return {};
    }

    std::string find(std::string_view theme, std::string_view shape, const std::vector<std::string>& dirs) {
        std::error_code          ec;
        std::vector<std::string> seen;
        // breadth first through what the theme inherits, then "default",
        // as libXcursor looks
        for (const std::string_view ROOT : {theme, std::string_view{"default"}}) {
            std::vector<std::pair<std::string, int>> queue{{std::string{ROOT}, 0}};
            for (size_t q = 0; q < queue.size(); ++q) {
                const auto [T, DEPTH] = queue[q];
                if (!plain(T) || std::ranges::contains(seen, T))
                    continue;
                seen.emplace_back(T);
                for (const auto NAME : namesOf(shape)) {
                    if (!plain(NAME))
                        continue;
                    for (const auto& d : dirs) {
                        auto path = d + "/" + T + "/cursors/" + std::string{NAME};
                        if (std::filesystem::is_regular_file(path, ec))
                            return path;
                    }
                }
                if (DEPTH >= MAX_DEPTH)
                    continue;
                for (const auto& d : dirs)
                    for (auto& parent : inheritsOf(d, T))
                        queue.emplace_back(std::move(parent), DEPTH + 1);
            }
        }
        return "";
    }
}
