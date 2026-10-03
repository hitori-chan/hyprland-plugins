// awesome/core/store.cpp — see store.hpp.
#include "store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace NAwesome::detail {

    std::string readBoundedFile(const fs::path& path) {
        std::error_code statusError;
        if (!fs::is_regular_file(path, statusError))
            return {};
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};
        std::string contents(MAX_STORE_FILE_BYTES + 1, '\0');
        f.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        const auto READ = static_cast<size_t>(f.gcount());
        if (READ > MAX_STORE_FILE_BYTES)
            return {};
        contents.resize(READ);
        return contents;
    }

} // namespace NAwesome::detail

namespace NAwesome {

    std::filesystem::path stateBase() {
        const char* XDG  = std::getenv("XDG_STATE_HOME");
        const char* HOME = std::getenv("HOME");
        return XDG && *XDG ? fs::path{XDG} : fs::path{HOME ? HOME : ""} / ".local" / "state";
    }

    std::filesystem::path stateDir() {
        return stateBase() / "hyprland" / "plugin" / "awesome";
    }

    std::filesystem::path legacyStateDir() {
        return stateBase() / "awesome";
    }

    std::filesystem::path cacheBase() {
        const char* XDG  = std::getenv("XDG_CACHE_HOME");
        const char* HOME = std::getenv("HOME");
        return XDG && *XDG ? fs::path{XDG} : fs::path{HOME ? HOME : ""} / ".cache";
    }

    static bool validKey(std::string_view k) {
        if (k.empty() || k.size() > MAX_STORE_KEY_BYTES)
            return false;
        return k.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    // ---- BoxStore ----

    bool BoxStore::remember(std::string_view cls, Box box) {
        if (!validKey(cls) || !validBox(box))
            return false;
        const std::string key{cls};
        if (const auto IT = rows.find(key); IT != rows.end()) {
            if (IT->second == box)
                return false;
            IT->second = box;
            return true;
        }
        if (rows.size() >= MAX_STORE_ENTRIES)
            return false;
        rows.emplace(key, box);
        return true;
    }

    bool BoxStore::parseRow(std::string_view line, BoxStore& out) {
        // "x y w h class", class LAST: app ids contain spaces and
        // colons, never tabs. Take the leading tab-terminated numbers;
        // whatever follows is the class, so a class starting with a
        // digit ("0ad") cannot be eaten.
        double      v[4] = {};
        int         n    = 0;
        const char* p    = line.data();
        const char* const END = line.data() + line.size();
        while (n < 4 && p < END) {
            char*        e   = nullptr;
            const double NUM = std::strtod(p, &e);
            if (e == p || *e != '\t')
                break;
            v[n++] = NUM;
            p      = e + 1;
        }
        // n == 2 is the position-only form (legacy stores wrote it);
        // it migrates with a zero size (module policy decides whether
        // a zero-size row is a usable restore target)
        if ((n != 2 && n != 4) || p == END)
            return false;
        for (int i = 0; i < 4; i++) {
            if (!std::isfinite(v[i]) || v[i] < (double)MIN_COORD || v[i] > (double)MAX_COORD)
                return false;
        }
        // the range check above keeps every value inside int; the cast
        // is explicit so -Wnarrowing stays honest about the domain.
        // The class is a pointer+length view into the buffer — never a
        // const char* (which would strlen past the line's end, since the
        // row has no terminator of its own).
        return out.remember(std::string_view{p, (size_t)(END - p)}, n == 4 ? Box{static_cast<int>(std::llround(v[0])), static_cast<int>(std::llround(v[1])), static_cast<int>(std::llround(v[2])), static_cast<int>(std::llround(v[3]))}
                                                                               : Box{static_cast<int>(std::llround(v[0])), static_cast<int>(std::llround(v[1])), 0, 0});
    }

    BoxStore BoxStore::read(const fs::path& path) {
        BoxStore out;
        const auto CONTENTS = detail::readBoundedFile(path);
        detail::forRows(CONTENTS, MAX_STORE_LINE_BYTES, MAX_STORE_ROWS, [&](std::string_view line) { parseRow(line, out); });
        return out;
    }

    void BoxStore::serializeRows(std::ostream& out, std::string_view prefix) const {
        size_t rows = 0;
        for (const auto& [CLS, B] : this->rows) {
            if (rows >= MAX_STORE_ENTRIES)
                break;
            if (!validKey(CLS) || !validBox(B))
                continue;
            out << prefix << B.x << '\t' << B.y << '\t' << B.w << '\t' << B.h << '\t' << CLS << '\n';
            ++rows;
        }
    }

    // ---- ListStore ----

    bool ListStore::remember(std::string_view entry) {
        if (entry.empty())
            return false;
        BoundedString<MAX_STORE_STRING_BYTES> B;
        B.assignClipped(entry);
        const auto& E = B.str();
        for (auto& EX : entries) {
            if (EX == E) {
                // re-running an entry moves it to most recent
                entries.erase(std::remove(entries.begin(), entries.end(), E), entries.end());
                break;
            }
        }
        entries.push_back(E);
        while (entries.size() > MAX_STORE_LIST_ENTRIES)
            entries.erase(entries.begin());
        return true;
    }

    bool ListStore::parseRow(std::string_view line, ListStore& out) {
        BoundedString<MAX_STORE_STRING_BYTES> B;
        B.assignClipped(line);
        if (B.str().empty())
            return false;
        out.entries.push_back(B.str());
        // bounded sliding window: an over-bound file keeps its MOST RECENT
        // entries (the tail), never the oldest head
        while (out.entries.size() > MAX_STORE_LIST_ENTRIES)
            out.entries.erase(out.entries.begin());
        return true;
    }

    ListStore ListStore::read(const fs::path& path) {
        ListStore out;
        const auto CONTENTS = detail::readBoundedFile(path);
        // lines ARE the data here (whole launcher queries), so the line gate
        // is the entry bound, not the generic 1 KiB guard.
        detail::forRows(CONTENTS, MAX_STORE_STRING_BYTES, MAX_STORE_ROWS, [&](std::string_view line) { parseRow(line, out); });
        return out;
    }

    void ListStore::serializeRows(std::ostream& out, std::string_view prefix) const {
        for (const auto& E : entries)
            out << prefix << E << '\n';
    }

    // ---- CountStore ----

    bool CountStore::bump(std::string_view name) {
        if (!validKey(name))
            return false;
        auto& C = counts[std::string{name}];
        if (C >= 1000000)
            return false; // the display only ranks; a hostile count file must not grow the file
        ++C;
        return true;
    }

    bool CountStore::parseRow(std::string_view line, CountStore& out) {
        // "name;count": the count is the LAST field, the name may contain
        // anything but a newline (app names with semicolons are possible —
        // only the trailing number is parsed)
        const auto SEP = line.rfind(';');
        if (SEP == std::string_view::npos || SEP == 0)
            return false;
        const auto NAME = std::string_view{line.data(), SEP};
        if (!validKey(NAME))
            return false;
        const char* BEG = line.data() + SEP + 1;
        const char* END = line.data() + line.size();
        char*        E   = nullptr;
        const long  C   = std::strtol(BEG, &E, 10);
        if (E != END || C < 0 || C > 1000000)
            return false;
        out.counts[std::string{NAME}] = (int)C;
        return true;
    }

    CountStore CountStore::read(const fs::path& path) {
        CountStore out;
        const auto CONTENTS = detail::readBoundedFile(path);
        detail::forRows(CONTENTS, MAX_STORE_LINE_BYTES, MAX_STORE_ROWS, [&](std::string_view line) { parseRow(line, out); });
        return out;
    }

    void CountStore::serializeRows(std::ostream& out, std::string_view prefix) const {
        size_t rows = 0;
        for (const auto& [N, C] : counts) {
            if (rows >= MAX_STORE_ENTRIES)
                break;
            if (!validKey(N) || C < 0)
                continue;
            out << prefix << N << ';' << C << '\n';
            ++rows;
        }
    }

} // namespace NAwesome
