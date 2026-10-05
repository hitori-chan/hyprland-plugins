// awesome/core/store.hpp — the bounded store types behind the plugin's
// single persistent state (core/state.hpp owns the file, its format, the
// load and the save path).
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
#include <iterator>
#include <list>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NAwesome {

    inline constexpr size_t MAX_STORE_KEY_BYTES = 512;
    inline constexpr size_t MAX_STORE_ENTRIES   = 1024; // per keyed store
    // per list entry: the launcher history stores whole queries, so the
    // entry bound is the prompt's own query cap (shell/menubar.cpp QUERY_MAX
    // = 4096) — the history can never hold more than the prompt admits.
    inline constexpr size_t MAX_STORE_STRING_BYTES = 4096;
    inline constexpr size_t MAX_STORE_LIST_ENTRIES = 50; // awful.prompt's history_max
    inline constexpr int    MAX_LAUNCH_COUNT       = 1000000;
    // one hostile file must not cost more than a bounded number of rows
    // to parse
    inline constexpr size_t MAX_STATE_ROWS = 16384;
    // what the writer can produce at the caps above (3 x 1024 rows of
    // 512-byte keys, 50 history rows of 4 KiB: ~1.9 MB): the reader must
    // accept anything the writer wrote
    inline constexpr size_t MAX_STATE_FILE_BYTES = 4u << 20;

    // A key: non-empty, bounded, free of the row format's separators (tab,
    // CR, LF) and NUL.
    bool validStoreKey(std::string_view k);

    inline bool validStoreValue(const Box& b) {
        return validBox(b);
    }
    inline bool validStoreValue(int count) {
        return count >= 0 && count <= MAX_LAUNCH_COUNT;
    }

    namespace detail {
        // One bounded read of the whole file; empty when absent,
        // oversized, or unreadable.
        std::string readBoundedFile(const std::filesystem::path& path, size_t maxBytes);

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

    // key → value, least recently remembered first. At the bound the oldest
    // key goes: refusing new keys once full would freeze the memory for
    // good, and app ids are not a closed set (every PWA, Wine program and
    // Steam game brings its own).
    template <typename V>
    class CRecentMap {
        using List = std::list<std::pair<std::string, V>>;

      public:
        CRecentMap() = default;
        // the index holds views into the list's own nodes: a copy would
        // point into the source
        CRecentMap(const CRecentMap&)            = delete;
        CRecentMap& operator=(const CRecentMap&) = delete;
        CRecentMap(CRecentMap&&)                 = default;
        CRecentMap& operator=(CRecentMap&&)      = default;

        const V* find(std::string_view k) const {
            const auto IT = m_index.find(k);
            return IT == m_index.end() ? nullptr : &IT->second->second;
        }
        bool contains(std::string_view k) const {
            return m_index.contains(k);
        }

        // Admits, updates and makes `k` the most recent. False when
        // invalid, or already the most recent with this value: the caller
        // dirties the state only on true.
        bool put(std::string_view k, const V& v) {
            if (!validStoreKey(k) || !validStoreValue(v))
                return false;
            if (const auto IT = m_index.find(k); IT != m_index.end()) {
                if (IT->second->second == v && std::next(IT->second) == m_list.end())
                    return false;
                IT->second->second = v;
                m_list.splice(m_list.end(), m_list, IT->second); // nodes and views stay put
                return true;
            }
            if (m_list.size() >= MAX_STORE_ENTRIES) {
                m_index.erase(m_list.front().first);
                m_list.pop_front();
            }
            m_list.emplace_back(std::string{k}, v);
            m_index.emplace(m_list.back().first, std::prev(m_list.end()));
            return true;
        }

        template <typename P>
        void eraseIf(P pred) {
            for (auto IT = m_list.begin(); IT != m_list.end();) {
                if (pred(IT->first, IT->second)) {
                    m_index.erase(IT->first);
                    IT = m_list.erase(IT);
                } else
                    ++IT;
            }
        }
        void clear() {
            m_index.clear();
            m_list.clear();
        }
        size_t size() const {
            return m_list.size();
        }
        bool empty() const {
            return m_list.empty();
        }
        // oldest first
        typename List::const_iterator begin() const {
            return m_list.begin();
        }
        typename List::const_iterator end() const {
            return m_list.end();
        }
        bool operator==(const CRecentMap& o) const {
            return m_list == o.m_list;
        }

      private:
        List                                                       m_list;
        std::unordered_map<std::string_view, typename List::iterator> m_index;
    };

    // One bounded list of bounded strings (the launcher's query history:
    // most recent last, re-running a query moves it to the most recent).
    struct ListStore {
        std::vector<std::string> entries;

        // Dedups, appends most-recent-last, bounds length and entry size.
        // False when nothing changed.
        bool remember(std::string_view entry);
    };

    // a launch: the count goes up and the name becomes the most recent
    bool bumpCount(CRecentMap<int>& counts, std::string_view name);

    // $XDG_STATE_HOME/hyprland/plugin/awesome/, ~/.local/state fallback:
    // the dir is this plugin's — family / category / plugin — bare
    // "awesome" collided with real AwesomeWM state.
    std::filesystem::path stateDir();

} // namespace NAwesome
