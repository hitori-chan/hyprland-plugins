// awesome/core/state.cpp — see state.hpp.
#include "state.hpp"

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

        // The four legacy row names inside one pre-unification state dir.
        constexpr const char* const LEGACY_FILES[4] = {"windows-spot.tsv", "windows-windowed.tsv", "shell-launches.tsv", "shell-history.tsv"};

        // Key-by-key merge: dst wins, src fills only missing keys (and
        // stays within the store's entry bound).
        void mergeBoxes(BoxStore& dst, const BoxStore& src) {
            for (const auto& [K, B] : src.rows)
                if (!dst.rows.contains(K) && dst.rows.size() < MAX_STORE_ENTRIES)
                    dst.rows.emplace(K, B);
        }
        void mergeCounts(CountStore& dst, const CountStore& src) {
            for (const auto& [K, C] : src.counts)
                if (!dst.counts.contains(K) && dst.counts.size() < MAX_STORE_ENTRIES)
                    dst.counts.emplace(K, C);
        }

        // One pre-unification four-file layout (newest dir first).
        void mergeLegacyLayout(const fs::path& dir, AppState& m) {
            mergeBoxes(m.spot, BoxStore::read(dir / LEGACY_FILES[0]));
            mergeBoxes(m.windowed, BoxStore::read(dir / LEGACY_FILES[1]));
            mergeCounts(m.launches, CountStore::read(dir / LEGACY_FILES[2]));
            if (m.history.entries.empty())
                m.history = ListStore::read(dir / LEGACY_FILES[3]);
        }

        // The migration's dead sources, in load order. Removed only after
        // state.tsv is safely written; each removal is best-effort (a
        // read-only or busy source must not block the load).
        const std::vector<fs::path>& sources() {
            static const std::vector<fs::path> S = {
                stateDir() / LEGACY_FILES[0],  stateDir() / LEGACY_FILES[1],  stateDir() / LEGACY_FILES[2],  stateDir() / LEGACY_FILES[3],
                legacyStateDir() / LEGACY_FILES[0], legacyStateDir() / LEGACY_FILES[1], legacyStateDir() / LEGACY_FILES[2],
                legacyStateDir() / LEGACY_FILES[3], stateBase() / "hyprplace" / "lastspot.tsv",   stateBase() / "hyprmax" / "windowed.tsv",
                cacheBase() / "hyprbar" / "menu_count_file",  cacheBase() / "hyprbar" / "history_menu",
            };
            return S;
        }

        void consumeSources() {
            std::error_code ec;
            for (const auto& P : sources())
                fs::remove(P, ec);
            // the emptied ancestors: the pre-rename dir, the ancient
            // per-module dirs. Only rmdir what is actually empty.
            for (const auto& D : {legacyStateDir(), stateBase() / "hyprplace", stateBase() / "hyprmax", cacheBase() / "hyprbar"})
                fs::remove(D, ec);
        }

    } // namespace

    std::filesystem::path StateStore::path() {
        return stateDir() / "state.tsv";
    }

    bool StateStore::readUnified(const fs::path& path, AppState& out) {
        const auto CONTENTS = detail::readBoundedFile(path, MAX_STATE_FILE_BYTES);
        if (CONTENTS.empty())
            return false;
        size_t perKind[4] = {}; // 0 spot, 1 windowed, 2 launches, 3 history
        constexpr size_t   KIND_CAP[4] = {MAX_STORE_ENTRIES, MAX_STORE_ENTRIES, MAX_STORE_ENTRIES, MAX_STORE_LIST_ENTRIES};
        // a row is its kind prefix plus a payload of up to a whole string
        detail::forRows(CONTENTS, MAX_STORE_STRING_BYTES + 64, MAX_STATE_ROWS, [&](std::string_view line) {
            const auto T = line.find('\t');
            if (T == std::string_view::npos)
                return;
            const auto KIND = std::string_view{line.data(), T};
            const auto PAYLOAD = line.substr(T + 1);
            if (KIND == "spot") {
                if (perKind[0] < KIND_CAP[0])
                    perKind[0] += BoxStore::parseRow(PAYLOAD, out.spot);
            } else if (KIND == "windowed") {
                if (perKind[1] < KIND_CAP[1])
                    perKind[1] += BoxStore::parseRow(PAYLOAD, out.windowed);
            } else if (KIND == "launches") {
                if (perKind[2] < KIND_CAP[2])
                    perKind[2] += CountStore::parseRow(PAYLOAD, out.launches);
            } else if (KIND == "history") {
                // parseRow bounds the store itself (most-recent window)
                ListStore::parseRow(PAYLOAD, out.history);
            }
        });
        return true;
    }

    bool StateStore::writeUnified(const fs::path& path, const AppState& data) {
        std::ostringstream out;
        data.spot.serializeRows(out, "spot\t");
        data.windowed.serializeRows(out, "windowed\t");
        data.launches.serializeRows(out, "launches\t");
        data.history.serializeRows(out, "history\t");
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
        AppState        U{};
        std::error_code ec;
        const auto      STATUS = fs::status(P, ec);
        if (fs::exists(STATUS) || (ec && ec != std::errc::no_such_file_or_directory)) {
            if (readUnified(P, U) || (fs::is_regular_file(STATUS) && fs::file_size(P, ec) == 0 && !ec)) {
                m_data = std::move(U);
                return; // the unified file is live; nothing to migrate
            }
            // there, but unreadable (EIO, EACCES, mangled past the bounds):
            // never clobber it with a migration or this session's state —
            // a failed read once looked like a missing file and the user's
            // memory was overwritten empty
            m_writeBlocked = true;
            return;
        }

        // Migration, newest first: the current four-file layout, the
        // pre-rename layout, the ancient per-module stores.
        AppState M{};
        mergeLegacyLayout(stateDir(), M);
        mergeLegacyLayout(legacyStateDir(), M);
        mergeBoxes(M.spot, BoxStore::read(stateBase() / "hyprplace" / "lastspot.tsv"));
        mergeBoxes(M.windowed, BoxStore::read(stateBase() / "hyprmax" / "windowed.tsv"));
        mergeCounts(M.launches, CountStore::read(cacheBase() / "hyprbar" / "menu_count_file"));
        if (M.history.entries.empty())
            M.history = ListStore::read(cacheBase() / "hyprbar" / "history_menu");

        m_data = std::move(M);
        if (writeUnified(P, m_data))
            consumeSources();
        // a failed first write keeps the in-memory copy live; the next
        // dirty() retries the same unified write.
    }

    void StateStore::dirty() {
        m_saver.dirty();
    }
    void StateStore::flush() {
        m_saver.flush();
    }

    StateStore::StateStore() : m_saver([this]() {
        if (!this->m_writeBlocked)
            this->writeUnified(path(), this->m_data);
    }) {}

} // namespace NAwesome
