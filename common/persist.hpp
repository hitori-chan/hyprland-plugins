// common/persist.hpp — plugin state that survives relogs: the XDG state
// path, the atomic write (temp + rename(2) — a crash mid-write must not eat
// the store, and an in-place rewrite of a mapped file is crash class 5),
// and the save coalescer (a mass close at logout must not storm the disk
// with one full rewrite per window).
#pragma once

#include "lifecycle.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>

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

    // ---- the per-class geometry stores (hyprplace's spots, hyprmax's
    // windowed boxes) ----
    //
    // One row per app class: "x y w h class", TAB separated, the class LAST so
    // any app_id parses (they contain spaces and colons, never tabs). Rows of
    // three fields are the legacy position-only form and load with a zero
    // size. Unparseable rows are skipped, never fatal: the file is user-facing
    // state and a hostile one must not take the session down.
    using SBoxStore = std::unordered_map<std::string, CBox>;

    inline SBoxStore readBoxTsv(const std::filesystem::path& path) {
        SBoxStore     out;
        std::ifstream f(path);
        std::string   line;
        while (std::getline(f, line)) {
            // take the leading tab-terminated numbers; whatever follows is the
            // class, so a class starting with a digit ("0ad") can't be eaten
            double      v[4] = {};
            int         n    = 0;
            const char* p    = line.c_str();
            while (n < 4) {
                char*        e   = nullptr;
                const double NUM = std::strtod(p, &e);
                if (e == p || *e != '\t')
                    break;
                v[n++] = NUM;
                p      = e + 1;
            }
            if ((n != 2 && n != 4) || !*p)
                continue; // neither form, or no class
            out[p] = n == 4 ? CBox{v[0], v[1], v[2], v[3]} : CBox{v[0], v[1], 0, 0};
        }
        return out;
    }

    inline bool writeBoxTsv(const std::filesystem::path& path, const SBoxStore& store) {
        std::ostringstream out;
        for (const auto& [CLS, B] : store)
            out << std::llround(B.x) << '\t' << std::llround(B.y) << '\t' << std::llround(B.w) << '\t' << std::llround(B.h) << '\t' << CLS << '\n';
        return writeAtomic(path, out.str());
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
