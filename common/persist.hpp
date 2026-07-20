// common/persist.hpp — plugin state that survives relogs: the XDG state
// path, the atomic write (temp + rename(2) — a crash mid-write must not eat
// the store, and an in-place rewrite of a mapped file is crash class 5),
// and the save coalescer (a mass close at logout must not storm the disk
// with one full rewrite per window).
#pragma once

#include "lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace NHyprCommon {

    // $XDG_STATE_HOME/<component>/<file>, ~/.local/state fallback
    inline std::filesystem::path statePath(const char* component, const char* file) {
        const char* XDG  = std::getenv("XDG_STATE_HOME");
        const char* HOME = std::getenv("HOME");
        const auto  BASE = XDG && *XDG ? std::filesystem::path{XDG} : std::filesystem::path{HOME ? HOME : ""} / ".local/state";
        return BASE / component / file;
    }

    inline bool writeAtomic(const std::filesystem::path& path, const std::string& contents) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto TMP = path.string() + ".tmp";
        {
            std::ofstream f(TMP, std::ios::trunc);
            if (!f)
                return false;
            f << contents;
        }
        std::filesystem::rename(TMP, path, ec);
        return !ec;
    }

    // Many dirty marks, one deferred write. PLUGIN_EXIT calls flush() after
    // the lifecycle reset: the queued hop never fires at compositor exit,
    // but the dirty state must still reach the disk.
    class CSaver {
      public:
        explicit CSaver(std::function<void()> write) : m_write(std::move(write)) {}

        void dirty() {
            if (m_queued)
                return; // one drain coalesces a burst — re-arming would cancel it
            m_queued = true;
            m_hop.arm([this]() {
                m_queued = false;
                m_write();
            });
        }
        void flush() {
            if (!m_queued)
                return;
            m_queued = false;
            m_write();
        }

      private:
        CHop                  m_hop;
        bool                  m_queued = false;
        std::function<void()> m_write;
    };

} // namespace NHyprCommon
