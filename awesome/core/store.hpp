// awesome/core/store.hpp — persistent plugin state under
// $XDG_STATE_HOME/awesome/, bounded by admission.
//
// One file per store; every row is admitted or skipped, never fatal — the
// file is user-facing state and a hostile one must not take the session
// down. Writes are atomic (temp + rename(2)): a crash mid-write must not
// eat the store.
#pragma once

#include "box.hpp"
#include "bounded.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NAwesome {

    // Shared admission bounds (the legacy per-plugin stores' contract,
    // kept so migration is behavior-identical).
    inline constexpr size_t MAX_STORE_FILE_BYTES    = 1024 * 1024;
    inline constexpr size_t MAX_STORE_LINE_BYTES    = 1024;
    inline constexpr size_t MAX_STORE_ROWS          = 4096;
    inline constexpr size_t MAX_STORE_KEY_BYTES     = 512;
    inline constexpr size_t MAX_STORE_ENTRIES       = 1024;
    inline constexpr size_t MAX_STORE_STRING_BYTES  = 512; // per list entry
    inline constexpr size_t MAX_STORE_LIST_ENTRIES  = 50;  // awful.prompt's history_max

    // app class → remembered box (windowed sizes, spawn spots)
    struct BoxStore {
        std::unordered_map<std::string, Box> rows;

        // Admission + change detection in one: false = invalid, unchanged,
        // or full; the caller dirties the store only on true.
        bool remember(std::string_view cls, Box box);
        bool contains(std::string_view cls) const {
            return rows.contains(std::string{cls});
        }
        Box* find(std::string_view cls) {
            const auto IT = rows.find(std::string{cls});
            return IT == rows.end() ? nullptr : &IT->second;
        }
        const Box* find(std::string_view cls) const {
            const auto IT = rows.find(std::string{cls});
            return IT == rows.end() ? nullptr : &IT->second;
        }

        static BoxStore read(const std::filesystem::path& path);
        bool            write(const std::filesystem::path& path) const;
    };

    // One bounded list of bounded strings, one entry per line (the
    // launcher's query history: most recent last, re-running a query moves
    // it to the front of recency).
    struct ListStore {
        std::vector<std::string> entries;

        // Dedups, appends most-recent-last, bounds length and entry size.
        // False when nothing changed.
        bool remember(std::string_view entry);

        static ListStore read(const std::filesystem::path& path);
        bool             write(const std::filesystem::path& path) const;
    };

    // $XDG_STATE_HOME/awesome/, ~/.local/state/awesome fallback
    std::filesystem::path stateDir();

    // One-time migration: when the fresh file does not exist and a legacy
    // file does, load the legacy contents into the fresh path. Legacy
    // files are read, never modified — the old plugins are gone after
    // cutover, so the legacy file is dead state, not a live store.
    bool migrateBoxStore(const std::filesystem::path& fresh, const std::filesystem::path& legacy);
    bool migrateListStore(const std::filesystem::path& fresh, const std::filesystem::path& legacy);

} // namespace NAwesome
