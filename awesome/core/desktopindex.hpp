// awesome/core/desktopindex.hpp — the .desktop icon index: ONE scan of the
// XDG application dirs for the whole plugin. The bar (task and tray icons)
// and the cards (a sender's desktop-entry hint) both map a desktop id or a
// window class to the entry's Icon=; they used to run two identical scans,
// two helper processes and two maps.
//
// The scan runs in the file-index helper process (core/fileindex.hpp);
// batches fold in on the event loop, and subscribers hear once per batch
// that added entries. The supervisor owns it: started before the modules
// init, stopped (subscribers dropped) after they tear down.
#pragma once

#include "fileindex.hpp"

#include <hyprland/src/helpers/memory/Memory.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class CEventLoopTimer;

namespace NAwesome {

    inline std::string asciiLower(std::string value) {
        for (auto& c : value)
            c = (char)std::tolower((unsigned char)c);
        return value;
    }

    class CDesktopIcons {
      public:
        static CDesktopIcons& inst() {
            static CDesktopIcons I;
            return I;
        }

        void start();
        void stop();

        // the entry's Icon= for a LOWERCASED desktop-file basename or
        // StartupWMClass; nullptr while the scan has not reached it
        const std::string* find(std::string_view lowerKey) const {
            const auto IT = m_icons.find(std::string{lowerKey});
            return IT == m_icons.end() ? nullptr : &IT->second;
        }
        const std::unordered_map<std::string, std::string>& entries() const {
            return m_icons;
        }
        bool scanning() const {
            return m_scanning;
        }

        // after each batch that added entries; register from a module's
        // init (stop() drops them all)
        void subscribe(std::function<void()> fn) {
            m_subs.push_back(std::move(fn));
        }

      private:
        void poll();
        void index(const CAsyncFileIndex::SEntry& entry);

        CAsyncFileIndex                              m_index;
        std::unordered_map<std::string, std::string> m_icons;
        std::vector<std::function<void()>>           m_subs;
        SP<CEventLoopTimer>                          m_poll;
        uint64_t                                     m_generation = 0;
        bool                                         m_scanning   = false;
    };

} // namespace NAwesome
