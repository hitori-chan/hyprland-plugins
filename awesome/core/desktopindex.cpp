#include "desktopindex.hpp"
#include "desktop_exec.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <algorithm>
#include <filesystem>

namespace NAwesome {

    namespace {
        constexpr size_t MAX_DESKTOP_FILES   = 4096;
        constexpr size_t MAX_DESKTOP_VISITED = 65536;
        // files folded per poll: one is tens of microseconds (a group with
        // every translation is a thousand lines); fewer stretch a full scan
        // over seconds of fallback icons
        constexpr size_t FILES_PER_POLL = 32;
        // a hung mount in XDG_DATA_DIRS stalls the helper, not the loop; the
        // scan then gives up and keeps what it found instead of polling on
        constexpr auto SCAN_DEADLINE = std::chrono::seconds(30);

        std::vector<std::string> splitDirs(std::string_view s) {
            std::vector<std::string> out;
            for (size_t p = 0; p < s.size();) {
                const auto E = s.find(':', p);
                if (const auto D = s.substr(p, E == std::string_view::npos ? s.size() - p : E - p); !D.empty())
                    out.emplace_back(D);
                if (E == std::string_view::npos)
                    break;
                p = E + 1;
            }
            return out;
        }

        // the applications dirs in precedence order (user dirs first: their
        // ids win), flatpak's exports included whatever XDG_DATA_DIRS says
        std::vector<std::string> applicationDirs() {
            std::vector<std::string> dirs;
            const auto               addDir = [&](const std::string& d) {
                if (!d.empty() && std::ranges::find(dirs, d) == dirs.end())
                    dirs.push_back(d);
            };
            const char* HOME = std::getenv("HOME");
            if (const char* XDH = std::getenv("XDG_DATA_HOME"); XDH && *XDH)
                addDir(std::string{XDH} + "/applications");
            else if (HOME)
                addDir(std::string{HOME} + "/.local/share/applications");
            if (HOME)
                addDir(std::string{HOME} + "/.local/share/flatpak/exports/share/applications");
            const char* DATA_DIRS = std::getenv("XDG_DATA_DIRS");
            for (const auto& D : splitDirs(DATA_DIRS && *DATA_DIRS ? DATA_DIRS : "/usr/local/share:/usr/share"))
                addDir(D + (D.back() == '/' ? "applications" : "/applications"));
            addDir("/var/lib/flatpak/exports/share/applications");
            return dirs;
        }

        // the dirs' mtimes and their subdirs' (the scan recurses: an entry
        // added under wine/Programs/ changes only its own dir), bounded
        std::vector<int64_t> stampsOf(const std::vector<std::string>& dirs) {
            constexpr size_t     MAX_SUBDIRS = 64;
            std::vector<int64_t> out;
            out.reserve(dirs.size());
            const auto STAMP = [&](const std::filesystem::path& p) {
                std::error_code ec;
                const auto      T = std::filesystem::last_write_time(p, ec);
                out.push_back(ec ? 0 : (int64_t)T.time_since_epoch().count());
            };
            size_t subdirs = 0;
            for (const auto& D : dirs) {
                STAMP(D);
                std::error_code ec;
                // increment(ec): the range-for's operator++ throws
                for (auto it = std::filesystem::recursive_directory_iterator(D, std::filesystem::directory_options::skip_permission_denied, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator() && subdirs < MAX_SUBDIRS; it.increment(ec))
                    if (std::error_code dec; it->is_directory(dec)) {
                        STAMP(it->path());
                        subdirs++;
                    }
            }
            return out;
        }
    }

    void CDesktopIndex::start() {
        if (!g_pCompositor || !g_pEventLoopManager || !m_index.init(g_pCompositor->m_wlEventLoop))
            return;
        m_poll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { CDesktopIndex::inst().poll(); }, nullptr);
        g_pEventLoopManager->addTimer(m_poll);
        request(false);
    }

    void CDesktopIndex::stop() {
        if (m_poll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_poll);
        m_poll.reset();
        m_index.exit();
        m_live     = {};
        m_next     = {};
        m_subs.clear();
        m_scanning = m_aside = false;
    }

    void CDesktopIndex::refreshIfStale() {
        // not started, or a scan under way (it reads the dirs as they are)
        if (!m_poll || m_scanning)
            return;
        const auto DIRS = applicationDirs();
        if (DIRS == m_dirs && stampsOf(DIRS) == m_stamps)
            return;
        request(true);
    }

    void CDesktopIndex::request(bool aside) {
        m_dirs     = applicationDirs();
        m_stamps   = stampsOf(m_dirs);
        m_aside    = aside;
        // not `(aside ? m_next : m_live) = {}`: GCC 16.2 zero-fills an
        // aggregate assigned {} through a conditional, constructing nothing
        // (both tables kept 0 buckets: SIGFPE at the first insert)
        auto& SET  = aside ? m_next : m_live;
        SET        = SSet{};
        m_scanning = true;
        m_deadline = std::chrono::steady_clock::now() + SCAN_DEADLINE;
        CAsyncFileIndex::SRequest request;
        request.generation   = ++m_generation;
        request.extensions   = {".desktop"};
        request.maxEntries   = MAX_DESKTOP_FILES;
        request.maxVisited   = MAX_DESKTOP_VISITED;
        request.maxFileBytes = DesktopExec::MAX_DESKTOP_FILE_BYTES;
        request.recursive    = true; // a subdir's files are ids too (kde4/x.desktop is kde4-x)
        for (const auto& D : m_dirs)
            request.roots.emplace_back(D);
        m_index.request(std::move(request));
        if (m_poll)
            m_poll->updateTimeout(std::chrono::milliseconds(2));
    }

    void CDesktopIndex::poll() {
        if (!m_scanning)
            return;
        std::vector<CAsyncFileIndex::SEntry> files;
        const bool   COMPLETE = m_index.poll(m_generation, files, FILES_PER_POLL);
        auto&        SET      = m_aside ? m_next : m_live;
        const size_t BEFORE   = SET.entries.size();
        for (const auto& F : files)
            add(F, SET);
        m_scanning = !COMPLETE;
        // cut short: the helper failed, or the deadline gave up on it
        bool cut = COMPLETE && m_index.failed(m_generation);
        if (m_scanning && std::chrono::steady_clock::now() > m_deadline) {
            m_index.cancel(++m_generation);
            m_scanning = false;
            cut        = true;
        }
        // a cut scan proves nothing about the dirs: forget their stamps, so
        // the next refreshIfStale scans again instead of trusting a partial
        // index for the session
        if (cut)
            m_stamps.clear();
        if (m_poll)
            m_poll->updateTimeout(m_scanning ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
        if (!m_aside) {
            // and once as it ends: a subscriber may wait for the whole scan
            if (m_live.entries.size() != BEFORE || !m_scanning)
                notify();
            return;
        }
        if (m_scanning)
            return;
        // a refresh lands whole; one cut short keeps the live index (a hung
        // mount or a killed helper must not empty the launcher)
        if (!cut) {
            m_live = std::move(m_next);
            notify();
        }
        m_next  = {};
        m_aside = false;
    }

    void CDesktopIndex::notify() {
        ++m_version;
        for (const auto& fn : m_subs) {
            try {
                fn();
            } catch (...) {} // a timer callback: nothing may unwind into the loop
        }
    }

    // The file's [Desktop Entry] group (DesktopExec::parseEntry). Its Icon=
    // is keyed by the desktop id, the file's basename AND its
    // StartupWMClass: a window's class rarely equals its desktop-file name
    // (qBittorrent's is "qbittorrent" under
    // org.qbittorrent.qBittorrent.desktop). The first file wins an id and a
    // key (the dirs are walked in precedence order).
    void CDesktopIndex::add(const CAsyncFileIndex::SEntry& file, SSet& set) {
        std::string id = file.relative.generic_string();
        if (id.ends_with(".desktop"))
            id.resize(id.size() - 8);
        std::ranges::replace(id, '/', '-');
        if (id.empty() || set.ids.contains(id))
            return;

        auto PARSED = DesktopExec::parseEntry(file.contents);
        if (!PARSED)
            return;
        set.ids.insert(id);
        if (!PARSED->icon.empty())
            for (const auto& KEY : {id, file.path.stem().string(), PARSED->wmClass})
                if (!KEY.empty())
                    set.icons.try_emplace(asciiLower(KEY), PARSED->icon);
        set.entries.push_back(SDesktopEntry{std::move(*PARSED), std::move(id), file.path.string()});
    }

} // namespace NAwesome
