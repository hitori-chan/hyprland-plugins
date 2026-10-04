// awesome/core/desktopindex.cpp — see desktopindex.hpp.
#include "desktopindex.hpp"
#include "desktop_exec.hpp"
#include "icons.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <sstream>

namespace NAwesome {

    namespace {
        constexpr size_t MAX_DESKTOP_FILES   = 4096;
        constexpr size_t MAX_DESKTOP_VISITED = 16384;
        // entries folded per poll: parsing one is microseconds, and 8 per
        // 16 ms tick stretched a full scan over seconds of fallback icons
        constexpr size_t ENTRIES_PER_POLL = 32;
    }

    void CDesktopIcons::start() {
        if (!g_pCompositor || !g_pEventLoopManager || !m_index.init(g_pCompositor->m_wlEventLoop))
            return;
        m_poll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { CDesktopIcons::inst().poll(); }, nullptr);
        g_pEventLoopManager->addTimer(m_poll);

        m_icons.clear();
        m_scanning = true;
        CAsyncFileIndex::SRequest request;
        request.generation   = ++m_generation;
        request.extensions   = {".desktop"};
        request.maxEntries   = MAX_DESKTOP_FILES;
        request.maxVisited   = MAX_DESKTOP_VISITED;
        request.maxFileBytes = DesktopExec::MAX_DESKTOP_FILE_BYTES;
        for (auto& dir : xdgDataDirs())
            request.roots.emplace_back(dir + "/applications");
        m_index.request(std::move(request));
        m_poll->updateTimeout(std::chrono::milliseconds(2));
    }

    void CDesktopIcons::stop() {
        if (m_poll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_poll);
        m_poll.reset();
        m_index.exit();
        m_icons.clear();
        m_subs.clear();
        m_scanning = false;
    }

    void CDesktopIcons::poll() {
        if (!m_scanning)
            return;
        std::vector<CAsyncFileIndex::SEntry> entries;
        const bool   COMPLETE = m_index.poll(m_generation, entries, ENTRIES_PER_POLL);
        const size_t BEFORE   = m_icons.size();
        for (const auto& entry : entries)
            index(entry);
        m_scanning = !COMPLETE;
        if (m_poll)
            m_poll->updateTimeout(m_scanning ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
        if (m_icons.size() != BEFORE)
            for (const auto& fn : m_subs)
                fn();
    }

    // Icon= and StartupWMClass= from the [Desktop Entry] group, keyed by the
    // file's basename AND its StartupWMClass: a window's class rarely equals
    // its desktop-file name (ente's class is "ente", its file
    // ente-desktop.desktop; qBittorrent's is "qbittorrent" under
    // org.qbittorrent.qBittorrent.desktop). The first entry wins a key (the
    // XDG dirs are walked in precedence order).
    void CDesktopIcons::index(const CAsyncFileIndex::SEntry& entry) {
        std::istringstream F(entry.contents);
        std::string        icon, wmClass, line;
        bool               inEntry = false;
        while (std::getline(F, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.starts_with("[")) {
                if (inEntry)
                    break;
                inEntry = line == "[Desktop Entry]";
                continue;
            }
            if (!inEntry)
                continue;
            if (icon.empty() && line.starts_with("Icon=")) {
                if (const auto VALUE = DesktopExec::unescapeString(std::string_view{line}.substr(5)))
                    icon = *VALUE;
            } else if (wmClass.empty() && line.starts_with("StartupWMClass=")) {
                if (const auto VALUE = DesktopExec::unescapeString(std::string_view{line}.substr(15)))
                    wmClass = *VALUE;
            }
            if (!icon.empty() && !wmClass.empty())
                break;
        }
        if (icon.empty())
            return;
        const auto remember = [&](const std::string& key) {
            if (!key.empty())
                m_icons.try_emplace(asciiLower(key), icon);
        };
        remember(entry.path.stem().string());
        remember(wmClass);
    }

} // namespace NAwesome
