// awesome/core/store.hpp — the admission-bounded store types behind the
// plugin's single persistent state (core/state.hpp owns the file, the
// load/migration, and the save path).
//
// Every row is admitted or skipped, never fatal — the state is
// user-facing and a hostile file must not take the session down. Reads
// are one bounded read of the whole file: a file that grows after open,
// and special files whose reported size is meaningless, are rejected as
// a unit instead of retaining an arbitrary prefix.
#pragma once

#include "box.hpp"
#include "bounded.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NAwesome {

    // Shared admission bounds (the one-time migration reads the legacy
    // stores through exactly this admission).
    inline constexpr size_t MAX_STORE_FILE_BYTES    = 1024 * 1024;
    inline constexpr size_t MAX_STORE_LINE_BYTES    = 1024;
    inline constexpr size_t MAX_STORE_ROWS          = 4096; // per file, per store
    inline constexpr size_t MAX_STORE_KEY_BYTES     = 512;
    inline constexpr size_t MAX_STORE_ENTRIES       = 1024;
    // per list entry: the launcher history stores whole queries, so the
    // entry bound is the prompt's own query cap (shell/menubar.cpp QUERY_MAX
    // = 4096) — the history can never hold more than the prompt admits.
    inline constexpr size_t MAX_STORE_STRING_BYTES  = 4096;
    inline constexpr size_t MAX_STORE_LIST_ENTRIES  = 50;  // awful.prompt's history_max
    // the unified state.tsv interleaves the four stores: one hostile file
    // must not cost more than a bounded number of rows to parse.
    inline constexpr size_t MAX_STATE_ROWS          = 16384;
    // what the writer can produce at the caps above (1024 spot + 1024
    // windowed + 1024 launches rows of 512-byte keys, 50 history rows of
    // 4 KiB: ~1.9 MB): the reader must accept anything the writer wrote
    inline constexpr size_t MAX_STATE_FILE_BYTES    = 4u << 20;

    namespace detail {
        // One bounded read of the whole file; empty when absent,
        // oversized, or unreadable.
        std::string readBoundedFile(const std::filesystem::path& path, size_t maxBytes = MAX_STORE_FILE_BYTES);

        // Iterate rows; the callback owns per-row admission. The line view
        // is built from the line's own start — advancing `begin` first
        // hands the callback the NEXT line's start with this line's length
        // (out of range).
        inline void forRows(const std::string& contents, size_t maxLineBytes, size_t maxRows,
                            const std::function<void(std::string_view)>& fn) {
            size_t begin = 0;
            size_t rows  = 0;
            while (begin < contents.size() && rows++ < maxRows) {
                const auto END  = contents.find('\n', begin);
                const auto LEN  = (END == std::string::npos ? contents.size() : END) - begin;
                const auto LINE = std::string_view{contents.data() + begin, LEN};
                // the NUL check stays inside the line: a search from `begin`
                // to the file's end per line was O(n^2) on a NUL-free file
                if (LEN <= maxLineBytes && LINE.find('\0') == std::string_view::npos)
                    fn(LINE);
                if (END == std::string::npos)
                    break;
                begin = END + 1;
            }
        }
    }

    // app class → remembered box (windowed sizes, spawn spots)
    struct BoxStore {
        std::unordered_map<std::string, Box> rows;

        // Admission + change detection in one: false = invalid, unchanged,
        // or full; the caller dirties the state only on true.
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

        // One "x\ty\tw\th\tclass" row: true when admitted.
        static bool parseRow(std::string_view line, BoxStore& out);

        // A legacy (pre-unification) one-store-per-file box store.
        static BoxStore read(const std::filesystem::path& path);
        // The rows, each optionally prefixed (state.cpp passes the
        // kind column); the caller bounds nothing — the store does.
        void serializeRows(std::ostream& out, std::string_view prefix = {}) const;
    };

    // name → launch count (the launcher's most-launched ranking), one
    // "name;count" row. The name is the .desktop Name= (it may contain
    // spaces and punctuation, never a semicolon or newline).
    struct CountStore {
        std::map<std::string, int> counts;

        // False when nothing changed.
        bool bump(std::string_view name);

        // One "name;count" row: true when admitted.
        static bool parseRow(std::string_view line, CountStore& out);

        // A legacy (pre-unification) one-store-per-file count store.
        static CountStore read(const std::filesystem::path& path);
        void serializeRows(std::ostream& out, std::string_view prefix = {}) const;
    };

    // One bounded list of bounded strings, one entry per line (the
    // launcher's query history: most recent last, re-running a query moves
    // it to the most recent).
    struct ListStore {
        std::vector<std::string> entries;

        // Dedups, appends most-recent-last, bounds length and entry size.
        // False when nothing changed.
        bool remember(std::string_view entry);

        // One entry (the whole line, clipped): true when admitted.
        static bool parseRow(std::string_view line, ListStore& out);

        // A legacy (pre-unification) one-store-per-file list store.
        static ListStore read(const std::filesystem::path& path);
        void serializeRows(std::ostream& out, std::string_view prefix = {}) const;
    };

    // $XDG_STATE_HOME/hyprland/plugin/awesome/, ~/.local/state fallback:
    // the dir is this plugin's — family / category / plugin — bare
    // "awesome" collided with real AwesomeWM state.
    std::filesystem::path stateDir();

    // the pre-rename state dir; a one-time migration source (state.cpp)
    std::filesystem::path legacyStateDir();

    // the XDG state root itself ($XDG_STATE_HOME or ~/.local/state): the
    // anchor for the ancient per-module stores (hyprplace/, hyprmax/)
    std::filesystem::path stateBase();

    // the XDG cache root ($XDG_CACHE_HOME or ~/.cache): the anchor for
    // the ancient hyprbar launcher cache (menu_count_file, history_menu)
    std::filesystem::path cacheBase();

} // namespace NAwesome
