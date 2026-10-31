// The .desktop index: ONE scan of the XDG application dirs for the whole
// plugin. The bar (task and tray icons) and the cards (a sender's
// desktop-entry hint) map a desktop id or a window class to the entry's
// Icon=, and the launcher lists the entries.
//
// The scan runs in the file-index helper process (core/fileindex.hpp);
// batches fold in on the event loop, and subscribers hear once per change.
// The first scan fills the index as it goes (icons land progressively); a
// refresh (an applications dir changed since: an app installed or removed)
// builds the next index aside and swaps it in whole, so a lookup never sees
// a half-scanned set; a scan cut short (deadline, helper failure) swaps
// nothing and is retried at the next refresh check. The supervisor owns it:
// started before the modules init, stopped (subscribers dropped) after they
// tear down.
#pragma once

#include "desktop_exec.hpp"
#include "fileindex.hpp"

#include <hyprland/src/helpers/memory/Memory.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CEventLoopTimer;

namespace NAwesome {

    inline std::string asciiLower(std::string value) {
        for (auto& c : value)
            c = (char)std::tolower((unsigned char)c);
        return value;
    }

    // one entry the index holds: the parsed group (desktop_exec.hpp) and
    // where it came from
    struct SDesktopEntry : DesktopExec::SEntry {
        std::string id;   // the path under its dir, '/' -> '-', without ".desktop"
        std::string path; // the file (%k)
    };

    class CDesktopIndex {
      public:
        static CDesktopIndex& inst() {
            static CDesktopIndex I;
            return I;
        }

        void start();
        void stop();
        // rescan when an applications dir changed since the last scan (the
        // launcher asks on open: an index is not built once per login)
        void refreshIfStale();

        // the entry's Icon= for a LOWERCASED desktop id, file basename or
        // StartupWMClass; nullptr while the scan has not reached it
        const std::string* findIcon(std::string_view lowerKey) const {
            const auto IT = m_live.icons.find(std::string{lowerKey});
            return IT == m_live.icons.end() ? nullptr : &IT->second;
        }
        const std::unordered_map<std::string, std::string>& icons() const {
            return m_live.icons;
        }
        // in precedence order, one per desktop id
        const std::vector<SDesktopEntry>& entries() const {
            return m_live.entries;
        }
        // bumps on every change subscribers hear of
        uint64_t version() const {
            return m_version;
        }
        bool scanning() const {
            return m_scanning;
        }

        // after each change; register from a module's init (stop() drops
        // them all)
        void subscribe(std::function<void()> fn) {
            m_subs.push_back(std::move(fn));
        }

      private:
        struct SSet {
            std::vector<SDesktopEntry>                   entries;
            std::unordered_map<std::string, std::string> icons;
            std::unordered_set<std::string>              ids;
        };

        void        request(bool aside);
        void        poll();
        void        notify();
        static void add(const CAsyncFileIndex::SEntry& file, SSet& set);

        CAsyncFileIndex                       m_index;
        SSet                                  m_live, m_next;
        std::vector<std::string>              m_dirs;
        std::vector<int64_t>                  m_stamps;
        std::vector<std::function<void()>>    m_subs;
        SP<CEventLoopTimer>                   m_poll;
        std::chrono::steady_clock::time_point m_deadline;
        uint64_t                              m_generation = 0, m_version = 0;
        bool                                  m_scanning = false, m_aside = false;
    };

} // namespace NAwesome
