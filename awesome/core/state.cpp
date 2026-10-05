// awesome/core/state.cpp — see state.hpp.
#include "state.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace NAwesome {

    namespace {

        // temp + rename(2): a crash mid-write must not eat the state
        bool writeAtomic(const fs::path& path, const std::string& contents) {
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
                                         // failure leaves the state alone
                    return false;
                }
            }
            fs::rename(TMP, path, ec);
            if (ec)
                fs::remove(TMP, ec); // no orphan beside the state
            return !ec;
        }

    } // namespace

    namespace {

        constexpr std::string_view HEADER = "awesome-state\t2"; // the format's version line

        bool numberStart(char c) {
            return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
        }

        // `n` (2: a position, 4: a box) leading tab-terminated numbers, then
        // the key: the rest of the row. strtod skips leading whitespace (the
        // line's own end included), so a field must START with its number or
        // a short row would borrow the next row's.
        bool parseBox(std::string_view row, int n, CRecentMap<Box>& out) {
            double            v[4] = {};
            const char*       p    = row.data();
            const char* const END  = row.data() + row.size();
            for (int i = 0; i < n; i++) {
                if (p >= END || !numberStart(*p))
                    return false;
                char* e = nullptr;
                v[i]    = std::strtod(p, &e);
                if (e == p || e >= END || *e != '\t' || !std::isfinite(v[i]) || v[i] < (double)MIN_COORD || v[i] > (double)MAX_COORD)
                    return false;
                p = e + 1;
            }
            if (p >= END)
                return false;
            // the range check keeps every value inside int
            const auto I = [&](int i) { return static_cast<int>(std::llround(v[i])); };
            return out.put(std::string_view{p, (size_t)(END - p)}, n == 4 ? Box{I(0), I(1), I(2), I(3)} : Box{I(0), I(1), 0, 0});
        }

        // "<count>\t<name>"
        bool parseCount(std::string_view row, CRecentMap<int>& out) {
            const auto T = row.find('\t');
            if (T == 0 || T == std::string_view::npos || T > 7)
                return false;
            int count = 0;
            for (size_t i = 0; i < T; i++) {
                if (row[i] < '0' || row[i] > '9')
                    return false;
                count = count * 10 + (row[i] - '0');
            }
            return out.put(row.substr(T + 1), count);
        }

    } // namespace

    std::filesystem::path StateStore::path() {
        return stateDir() / "state.tsv";
    }

    bool StateStore::read(const fs::path& path, AppState& out) {
        const auto CONTENTS = detail::readBoundedFile(path, MAX_STATE_FILE_BYTES);
        if (CONTENTS.empty())
            return false;
        // the version line first: any other file is not ours to read, nor
        // to rewrite
        const auto EOL = CONTENTS.find('\n');
        if (std::string_view{CONTENTS}.substr(0, EOL) != HEADER)
            return false;
        // a row is its kind plus a payload of up to a whole string; the
        // version line is a kind nobody reads
        detail::forRows(CONTENTS, MAX_STORE_STRING_BYTES + 64, MAX_STATE_ROWS, [&](std::string_view line) {
            const auto T = line.find('\t');
            if (T == std::string_view::npos)
                return;
            const auto KIND = line.substr(0, T);
            const auto ROW  = line.substr(T + 1);
            if (KIND == "spot")
                parseBox(ROW, 2, out.spot);
            else if (KIND == "windowed")
                parseBox(ROW, 4, out.windowed);
            else if (KIND == "launches")
                parseCount(ROW, out.launches);
            else if (KIND == "history")
                out.history.remember(ROW);
        });
        return true;
    }

    bool StateStore::write(const fs::path& path, const AppState& data) {
        std::ostringstream out;
        out << HEADER << '\n';
        for (const auto& [K, B] : data.spot)
            out << "spot\t" << B.x << '\t' << B.y << '\t' << K << '\n';
        for (const auto& [K, B] : data.windowed)
            out << "windowed\t" << B.x << '\t' << B.y << '\t' << B.w << '\t' << B.h << '\t' << K << '\n';
        for (const auto& [K, C] : data.launches)
            out << "launches\t" << C << '\t' << K << '\n';
        for (const auto& E : data.history.entries)
            out << "history\t" << E << '\n';
        return writeAtomic(path, out.str());
    }

    StateStore& StateStore::inst() {
        static StateStore S;
        return S;
    }

    AppState& StateStore::data() {
        return m_data;
    }
    const AppState& StateStore::data() const {
        return m_data;
    }

    void StateStore::load() {
        if (m_loaded)
            return;
        m_loaded = true;

        const auto      P = path();
        std::error_code ec;
        const auto      STATUS = fs::status(P, ec);
        if (!fs::exists(STATUS) && (!ec || ec == std::errc::no_such_file_or_directory))
            return; // a first run: the first dirty() writes the file
        AppState U{};
        if (read(P, U) || (fs::is_regular_file(STATUS) && fs::file_size(P, ec) == 0 && !ec)) {
            m_data = std::move(U);
            return;
        }
        // there, but unreadable (EIO, EACCES, mangled past the bounds, not
        // this version):
        // never clobber it with this session's state — a failed read once
        // looked like a missing file and the user's memory was overwritten
        // empty
        m_writeBlocked = true;
    }

    void StateStore::dirty() {
        m_saver.dirty();
    }
    void StateStore::flush() {
        m_saver.flush();
    }

    StateStore::StateStore() : m_saver([this]() {
        if (!this->m_writeBlocked)
            this->write(path(), this->m_data);
    }) {}

} // namespace NAwesome
