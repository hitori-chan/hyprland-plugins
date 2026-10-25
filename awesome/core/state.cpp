// awesome/core/state.cpp — see state.hpp.
#include "state.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace NAwesome {

    namespace {

        // temp + fsync + rename(2) + directory fsync: a crash or a power
        // loss mid-write must not eat the state — without the fsync, a
        // journaling filesystem may commit the rename before the data and
        // come back with a zero-length file. The temp is the process's own
        // (no other writer's rename takes it), created fresh with O_EXCL and
        // O_NOFOLLOW (a planted symlink is never written through), 0600 (the
        // state holds the launcher's history).
        bool writeAtomic(const fs::path& path, const std::string& contents) {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            const auto TMP = path.string() + ".tmp." + std::to_string((long long)getpid());
            ::unlink(TMP.c_str()); // a leftover of this pid's crash, or a link: removed, never followed
            const int FD = ::open(TMP.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (FD < 0)
                return false;
            bool   ok   = true;
            size_t done = 0;
            while (ok && done < contents.size()) {
                const ssize_t N = ::write(FD, contents.data() + done, contents.size() - done);
                if (N < 0 && errno == EINTR)
                    continue;
                ok = N > 0;
                done += ok ? (size_t)N : 0;
            }
            ok = ok && ::fdatasync(FD) == 0; // a full disk can report only here
            ok = ::close(FD) == 0 && ok;
            if (!ok || ::rename(TMP.c_str(), path.c_str()) != 0) {
                ::unlink(TMP.c_str()); // the temp exists precisely so a failure leaves the state alone
                return false;
            }
            if (const int DIR = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); DIR >= 0) {
                ::fsync(DIR); // the rename itself is durable
                ::close(DIR);
            }
            return true;
        }

    } // namespace

    // The disk half of a save runs on a writer thread: fdatasync waits on
    // the filesystem's journal, and behind a heavy writer (a build, a copy)
    // that is seconds — once per window close, with every frame and input
    // event queued behind it. One write in flight, the newest snapshot
    // wins; the thread exits when it runs dry. drain() joins it: teardown
    // never unloads the plugin under a running write, and a flushed save
    // is on disk when flush() returns.
    class CDiskWriter {
      public:
        ~CDiskWriter() {
            drain();
        }

        void post(fs::path path, std::string contents) {
            {
                std::lock_guard lk(m_lock);
                m_next = SJob{std::move(path), std::move(contents)};
                if (m_running)
                    return; // the running loop takes it next
            }
            drain(); // a thread that ran dry and exited
            m_running = true;
            try {
                // process signals stay with the compositor's threads: the
                // writer inherits a mask blocking all but the faults (a
                // crash in it still reaches the crash handler)
                sigset_t mask, old;
                sigfillset(&mask);
                for (const int SIG : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS, SIGABRT})
                    sigdelset(&mask, SIG);
                pthread_sigmask(SIG_SETMASK, &mask, &old);
                try {
                    m_thread = std::thread([this]() {
                        pthread_setname_np(pthread_self(), "awesome-state");
                        run();
                    });
                } catch (...) {
                    pthread_sigmask(SIG_SETMASK, &old, nullptr);
                    throw;
                }
                pthread_sigmask(SIG_SETMASK, &old, nullptr);
            } catch (...) {
                m_running = false; // no thread to be had: write it here
                run();
            }
        }

        void drain() {
            if (m_thread.joinable())
                m_thread.join();
        }

      private:
        struct SJob {
            fs::path    path;
            std::string contents;
        };

        void run() {
            for (;;) {
                std::optional<SJob> job;
                {
                    std::lock_guard lk(m_lock);
                    if (!m_next) {
                        m_running = false;
                        return;
                    }
                    job = std::move(m_next);
                    m_next.reset();
                }
                try {
                    writeAtomic(job->path, job->contents);
                } catch (...) {} // nothing may escape a thread: std::terminate
            }
        }

        std::mutex          m_lock;
        std::optional<SJob> m_next;
        bool                m_running = false; // under m_lock once a thread exists
        std::thread         m_thread;
    };

    namespace {

        bool numberStart(char c) {
            return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
        }

        // `n` (2: a position, 4: a box) leading tab-terminated numbers, then
        // the key: the rest of the row. strtod skips leading whitespace (the
        // line's own end included), so a field must START with its number or
        // a short row would borrow the next row's.
        bool parseBox(std::string_view row, int n, CRecentMap<Box>& out) {
            double            v[4] = {};
            const char*       p    = row.data();
            const char* const END  = row.data() + row.size();
            for (int i = 0; i < n; i++) {
                if (p >= END || !numberStart(*p))
                    return false;
                char* e = nullptr;
                v[i]    = std::strtod(p, &e);
                if (e == p || e >= END || *e != '\t' || !std::isfinite(v[i]) || v[i] < (double)MIN_COORD || v[i] > (double)MAX_COORD)
                    return false;
                p = e + 1;
            }
            if (p >= END)
                return false;
            // the range check keeps every value inside int
            const auto I = [&](int i) { return static_cast<int>(std::llround(v[i])); };
            return out.put(std::string_view{p, (size_t)(END - p)}, n == 4 ? Box{I(0), I(1), I(2), I(3)} : Box{I(0), I(1), 0, 0});
        }

        // "<count>\t<name>"
        bool parseCount(std::string_view row, CRecentMap<int>& out) {
            const auto T = row.find('\t');
            if (T == 0 || T == std::string_view::npos || T > 7)
                return false;
            int count = 0;
            for (size_t i = 0; i < T; i++) {
                if (row[i] < '0' || row[i] > '9')
                    return false;
                count = count * 10 + (row[i] - '0');
            }
            return out.put(row.substr(T + 1), count);
        }

        // "<unix seconds>\t<key>"
        bool parseTime(std::string_view row, CRecentMap<int64_t>& out) {
            const auto T = row.find('\t');
            if (T == 0 || T == std::string_view::npos || T > 12)
                return false;
            int64_t t = 0;
            for (size_t i = 0; i < T; i++) {
                if (row[i] < '0' || row[i] > '9')
                    return false;
                t = t * 10 + (row[i] - '0');
            }
            return out.put(row.substr(T + 1), t);
        }

    } // namespace

    std::filesystem::path StateStore::path() {
        return stateDir() / "state.tsv";
    }

    bool StateStore::read(const fs::path& path, AppState& out) {
        const auto CONTENTS = detail::readBoundedFile(path, MAX_STATE_FILE_BYTES);
        if (CONTENTS.empty())
            return false;
        // a row is its kind plus a payload of up to a whole string
        detail::forRows(CONTENTS, MAX_STORE_STRING_BYTES + 64, MAX_STATE_ROWS, [&](std::string_view line) {
            const auto T = line.find('\t');
            if (T == std::string_view::npos)
                return;
            const auto KIND = line.substr(0, T);
            const auto ROW  = line.substr(T + 1);
            if (KIND == "spot")
                parseBox(ROW, 2, out.spot);
            else if (KIND == "windowed")
                parseBox(ROW, 4, out.windowed);
            else if (KIND == "launches")
                parseCount(ROW, out.launches);
            else if (KIND == "history")
                out.history.remember(ROW);
            else if (KIND == "notify-app")
                parseCount(ROW, out.notifyApp);
            else if (KIND == "notify-mute")
                parseTime(ROW, out.notifyMute);
        });
        return true;
    }

    static std::string serialize(const AppState& data) {
        std::ostringstream out;
        for (const auto& [K, B] : data.spot)
            out << "spot\t" << B.x << '\t' << B.y << '\t' << K << '\n';
        for (const auto& [K, B] : data.windowed)
            out << "windowed\t" << B.x << '\t' << B.y << '\t' << B.w << '\t' << B.h << '\t' << K << '\n';
        for (const auto& [K, C] : data.launches)
            out << "launches\t" << C << '\t' << K << '\n';
        for (const auto& E : data.history.entries)
            out << "history\t" << E << '\n';
        for (const auto& [K, F] : data.notifyApp)
            out << "notify-app\t" << F << '\t' << K << '\n';
        for (const auto& [K, T] : data.notifyMute)
            out << "notify-mute\t" << T << '\t' << K << '\n';
        return std::move(out).str();
    }

    bool StateStore::write(const fs::path& path, const AppState& data) {
        return writeAtomic(path, serialize(data));
    }

    StateStore& StateStore::inst() {
        static StateStore S;
        return S;
    }

    AppState& StateStore::data() {
        return m_data;
    }
    const AppState& StateStore::data() const {
        return m_data;
    }

    void StateStore::load() {
        if (m_loaded)
            return;
        m_loaded = true;

        const auto      P = path();
        std::error_code ec;
        const auto      STATUS = fs::status(P, ec);
        if (!fs::exists(STATUS) && (!ec || ec == std::errc::no_such_file_or_directory))
            return; // a first run: the first dirty() writes the file
        AppState U{};
        if (read(P, U) || (fs::is_regular_file(STATUS) && fs::file_size(P, ec) == 0 && !ec)) {
            m_data = std::move(U);
            return;
        }
        // there, but unreadable (EIO, EACCES, mangled past the bounds):
        // never clobber it with this session's state — a failed read once
        // looked like a missing file and the user's memory was overwritten
        // empty
        m_writeBlocked = true;
    }

    void StateStore::dirty() {
        m_saver.dirty();
    }
    void StateStore::flush() {
        m_saver.flush();
        m_writer->drain();
    }

    StateStore::StateStore() : m_writer(std::make_unique<CDiskWriter>()), m_saver([this]() {
        if (!this->m_writeBlocked)
            this->m_writer->post(path(), serialize(this->m_data));
    }) {}

    StateStore::~StateStore() = default;

} // namespace NAwesome
