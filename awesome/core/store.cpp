// awesome/core/store.cpp — see store.hpp.
//
// The read path is a single bounded read: files that grow after open, and
// special files whose reported size is meaningless, are rejected as a unit
// instead of retaining an arbitrary prefix. Unparseable rows are skipped,
// never fatal.
#include "store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace NAwesome {

    std::filesystem::path stateDir() {
        const char* XDG  = std::getenv("XDG_STATE_HOME");
        const char* HOME = std::getenv("HOME");
        const auto  BASE = XDG && *XDG ? fs::path{XDG} : fs::path{HOME ? HOME : ""} / ".local" / "state";
        return BASE / "awesome";
    }

    static bool validKey(std::string_view k) {
        if (k.empty() || k.size() > MAX_STORE_KEY_BYTES)
            return false;
        return k.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    // temp + rename(2): a crash mid-write must not eat the store
    static bool writeAtomic(const fs::path& path, const std::string& contents) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const auto TMP = path.string() + ".tmp";
        {
            std::ofstream f(TMP, std::ios::trunc);
            if (!f)
                return false;
            f << contents;
            f.close(); // flush BEFORE the check: a short write (full disk)
                       // reports only once the stream has tried to make it
            if (!f) {
                fs::remove(TMP, ec); // the temp exists precisely so a
                                     // failure leaves the store alone
                return false;
            }
        }
        fs::rename(TMP, path, ec);
        if (ec)
            fs::remove(TMP, ec); // no orphan beside the store
        return !ec;
    }

    // One bounded read of the whole file; empty when absent, oversized, or
    // unreadable.
    static std::string readBoundedFile(const fs::path& path) {
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

    // Iterate rows; the callback owns per-row admission. The line view is
    // built from the line's own start — advancing `begin` first hands the
    // callback the NEXT line's start with this line's length (out of range).
    template <typename F>
    static void forRows(const std::string& contents, F&& fn) {
        size_t begin = 0;
        size_t rows  = 0;
        while (begin < contents.size() && rows++ < MAX_STORE_ROWS) {
            const auto END = contents.find('\n', begin);
            const auto LEN = (END == std::string::npos ? contents.size() : END) - begin;
            const auto NUL = contents.find('\0', begin);
            if (LEN <= MAX_STORE_LINE_BYTES && (NUL == std::string::npos || NUL >= END))
                fn(std::string_view{contents.data() + begin, LEN});
            if (END == std::string::npos)
                break;
            begin = END + 1;
        }
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

    BoxStore BoxStore::read(const fs::path& path) {
        BoxStore out;
        const auto CONTENTS = readBoundedFile(path);
        forRows(CONTENTS, [&](std::string_view line) {
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
            // n == 2 is the legacy position-only form; it migrates with a
            // zero size (module policy decides whether a zero-size row is a
            // usable restore target)
            if ((n != 2 && n != 4) || p == END)
                return;
            for (int i = 0; i < 4; i++) {
                if (!std::isfinite(v[i]) || v[i] < (double)MIN_COORD || v[i] > (double)MAX_COORD)
                    return;
            }
            // the range check above keeps every value inside int; the cast
            // is explicit so -Wnarrowing stays honest about the domain.
            // The class is a pointer+length view into the buffer — never a
            // const char* (which would strlen past the line's end, since the
            // row has no terminator of its own).
            out.remember(std::string_view{p, (size_t)(END - p)}, n == 4 ? Box{static_cast<int>(std::llround(v[0])), static_cast<int>(std::llround(v[1])), static_cast<int>(std::llround(v[2])), static_cast<int>(std::llround(v[3]))}
                                                                         : Box{static_cast<int>(std::llround(v[0])), static_cast<int>(std::llround(v[1])), 0, 0});
        });
        return out;
    }

    bool BoxStore::write(const fs::path& path) const {
        std::ostringstream out;
        size_t             rows = 0;
        for (const auto& [CLS, B] : this->rows) {
            if (rows >= MAX_STORE_ENTRIES)
                break;
            if (!validKey(CLS) || !validBox(B))
                continue;
            out << B.x << '\t' << B.y << '\t' << B.w << '\t' << B.h << '\t' << CLS << '\n';
            ++rows;
        }
        return writeAtomic(path, out.str());
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

    ListStore ListStore::read(const fs::path& path) {
        ListStore out;
        const auto CONTENTS = readBoundedFile(path);
        forRows(CONTENTS, [&](std::string_view line) {
            BoundedString<MAX_STORE_STRING_BYTES> B;
            B.assignClipped(line);
            if (!B.str().empty())
                out.entries.push_back(B.str());
        });
        while (out.entries.size() > MAX_STORE_LIST_ENTRIES)
            out.entries.erase(out.entries.begin());
        return out;
    }

    bool ListStore::write(const fs::path& path) const {
        std::ostringstream out;
        for (const auto& E : entries)
            out << E << '\n';
        return writeAtomic(path, out.str());
    }

    // ---- migration (one-time; legacy files are read, never modified) ----

    static bool migrate(const fs::path& fresh, const fs::path& legacy, auto readFn, auto writeFn) {
        std::error_code ec;
        if (fs::exists(fresh, ec))
            return true; // the fresh store is live; the legacy file is dead
        if (!fs::is_regular_file(legacy, ec))
            return true; // nothing to migrate
        const auto STORE = readFn(legacy);
        return writeFn(fresh, STORE);
    }

    bool migrateBoxStore(const fs::path& fresh, const fs::path& legacy) {
        return migrate(fresh, legacy, [](const fs::path& P) { return BoxStore::read(P); },
                       [](const fs::path& P, const BoxStore& S) { return S.write(P); });
    }

    bool migrateListStore(const fs::path& fresh, const fs::path& legacy) {
        return migrate(fresh, legacy, [](const fs::path& P) { return ListStore::read(P); },
                       [](const fs::path& P, const ListStore& S) { return S.write(P); });
    }

} // namespace NAwesome
