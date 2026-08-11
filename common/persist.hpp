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
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
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
            f.close(); // flush BEFORE the check: the stream reports a short
                       // write (a full disk) only once it has tried to make it
            if (!f) {
                // the temp exists precisely so a failure leaves the store
                // alone — renaming half a file over it is the one outcome
                // this function is here to prevent
                std::filesystem::remove(TMP, ec);
                return false;
            }
        }
        std::filesystem::rename(TMP, path, ec);
        if (!ec)
            return true;
        std::error_code rm;
        std::filesystem::remove(TMP, rm); // no orphan beside the store
        return false;
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

    inline constexpr size_t MAX_BOX_STORE_FILE_BYTES = 1024 * 1024;
    inline constexpr size_t MAX_BOX_STORE_LINE_BYTES = 1024;
    inline constexpr size_t MAX_BOX_STORE_ROWS       = 4096;
    inline constexpr size_t MAX_BOX_STORE_CLASS_BYTES = 512;
    inline constexpr size_t MAX_BOX_STORE_ENTRIES    = 1024;

    inline bool validBoxNumber(double value) {
        // writeBoxTsv uses llround; stay strictly inside its representable
        // domain instead of letting hostile state invoke undefined behavior.
        return std::isfinite(value) && value > (double)std::numeric_limits<long long>::min() && value < (double)std::numeric_limits<long long>::max();
    }

    inline bool validBoxClass(std::string_view cls) {
        if (cls.empty() || cls.size() > MAX_BOX_STORE_CLASS_BYTES)
            return false;
        return cls.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    // The same admission path owns loaded and newly remembered state. False
    // means invalid, unchanged, or full; callers only need to dirty the store
    // when this returns true.
    inline bool rememberBox(SBoxStore& store, std::string_view cls, const CBox& box) {
        if (!validBoxClass(cls) || !validBoxNumber(box.x) || !validBoxNumber(box.y) || !validBoxNumber(box.w) || !validBoxNumber(box.h) || box.w < 0 || box.h < 0)
            return false;

        const std::string key{cls};
        if (const auto IT = store.find(key); IT != store.end()) {
            if (IT->second == box)
                return false;
            IT->second = box;
            return true;
        }
        if (store.size() >= MAX_BOX_STORE_ENTRIES)
            return false;
        store.emplace(key, box);
        return true;
    }

    inline SBoxStore readBoxTsv(const std::filesystem::path& path) {
        SBoxStore     out;
        std::error_code statusError;
        if (!std::filesystem::is_regular_file(path, statusError))
            return out;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return out;

        // One bounded read also covers files that grow after open and special
        // files whose reported size is not meaningful. Oversized input is
        // rejected as a unit instead of retaining an arbitrary prefix.
        std::string contents(MAX_BOX_STORE_FILE_BYTES + 1, '\0');
        f.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        const auto READ = static_cast<size_t>(f.gcount());
        if (READ > MAX_BOX_STORE_FILE_BYTES)
            return out;
        contents.resize(READ);

        size_t begin = 0;
        size_t rows  = 0;
        while (begin < contents.size() && rows++ < MAX_BOX_STORE_ROWS) {
            const auto END = contents.find('\n', begin);
            const auto LEN = (END == std::string::npos ? contents.size() : END) - begin;
            if (LEN > MAX_BOX_STORE_LINE_BYTES) {
                if (END == std::string::npos)
                    break;
                begin = END + 1;
                continue;
            }
            const std::string line{contents.data() + begin, LEN};
            if (line.find('\0') != std::string::npos) {
                if (END == std::string::npos)
                    break;
                begin = END + 1;
                continue;
            }
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
            if ((n != 2 && n != 4) || !*p) {
                if (END == std::string::npos)
                    break;
                begin = END + 1;
                continue; // neither form, or no class
            }
            bool valid = true;
            for (int i = 0; i < n; i++)
                valid = valid && validBoxNumber(v[i]);
            if (valid && (n != 4 || (v[2] >= 0 && v[3] >= 0)))
                rememberBox(out, p, n == 4 ? CBox{v[0], v[1], v[2], v[3]} : CBox{v[0], v[1], 0, 0});
            if (END == std::string::npos)
                break;
            begin = END + 1;
        }
        return out;
    }

    inline bool writeBoxTsv(const std::filesystem::path& path, const SBoxStore& store) {
        std::ostringstream out;
        size_t             rows = 0;
        for (const auto& [CLS, B] : store) {
            if (rows >= MAX_BOX_STORE_ENTRIES)
                break;
            if (!validBoxNumber(B.x) || !validBoxNumber(B.y) || !validBoxNumber(B.w) || !validBoxNumber(B.h) || B.w < 0 || B.h < 0 || !validBoxClass(CLS))
                continue;
            out << std::llround(B.x) << '\t' << std::llround(B.y) << '\t' << std::llround(B.w) << '\t' << std::llround(B.h) << '\t' << CLS << '\n';
            ++rows;
        }
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
