// common/fileindex.hpp -- bounded file enumeration without plugin-owned workers.
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

extern char** environ;

namespace NHyprCommon {

    // A plugin cannot leave a filesystem thread alive across dlclose, and a
    // jthread destructor can wait indefinitely in a hostile mount. Keep the
    // slow traversal in an owned helper process instead. Its only output is a
    // bounded record stream drained by the compositor's event loop.
    class CAsyncFileIndex {
      public:
        struct SEntry {
            std::filesystem::path path;
            std::filesystem::path relative;
            std::string           contents;
            bool                  directory = false;
        };

        struct SRequest {
            uint64_t                           generation   = 0;
            std::vector<std::filesystem::path> roots;
            std::vector<std::string>           extensions;
            std::string                        namePrefix;
            size_t                             maxEntries   = 0;
            size_t                             maxVisited   = 0;
            size_t                             maxFileBytes = 0; // zero leaves contents empty
            bool                               recursive    = false;
            bool                               executable   = false;
            bool                               directories  = false;
        };

        CAsyncFileIndex() = default;
        ~CAsyncFileIndex() {
            exit();
        }
        CAsyncFileIndex(const CAsyncFileIndex&)            = delete;
        CAsyncFileIndex& operator=(const CAsyncFileIndex&) = delete;

        // init/exit run on the compositor event loop. Requests made before
        // init deliberately complete empty instead of starting an unowned
        // helper while plugin startup is incomplete.
        bool init(wl_event_loop* loop) {
            if (!loop || (m_loop && m_loop != loop))
                return false;
            m_loop = loop;
            return true;
        }

        void request(SRequest request) {
            stopHelper(true);
            clearResults();
            m_generation = request.generation;
            m_complete   = false;
            m_eof        = false;
            m_roots.clear();

            if (!m_loop || !prepare(request)) {
                m_complete = true;
                return;
            }

            for (auto& root : request.roots)
                if (!root.empty())
                    m_roots.push_back(std::move(root));
            if (m_roots.empty()) {
                m_complete = true;
                return;
            }

            m_maxFileBytes = request.maxFileBytes;
            if (!spawn(std::move(request)))
                m_complete = true;
        }

        // A cancellation is another generation, so an old helper's records
        // cannot be observed after the consumer has moved on.
        void cancel(uint64_t generation) {
            stopHelper(true);
            clearResults();
            m_roots.clear();
            m_generation = generation;
            m_complete   = true;
            m_eof        = false;
        }

        // Move at most max entries onto the event loop. True means this
        // generation is complete and no retained result remains to consume.
        bool poll(uint64_t generation, std::vector<SEntry>& out, size_t max) {
            if (generation != m_generation)
                return false;
            while (out.size() < max && !m_results.empty()) {
                m_resultBytes -= m_results.front().bytes;
                out.push_back(std::move(m_results.front().entry));
                m_results.pop_front();
            }
            if (!parseFrames())
                return true;
            resumeIfPossible();
            finishIfEof();
            return m_complete && m_results.empty();
        }

        bool active(uint64_t generation) const {
            return m_generation == generation && !m_complete;
        }

        void exit() {
            stopHelper(true);
            clearResults();
            m_roots.clear();
            m_generation   = 0;
            m_complete     = true;
            m_eof          = false;
            m_loop         = nullptr;
            m_maxFileBytes = 0;
        }

      private:
        static constexpr size_t MAX_QUEUED_RESULTS = 128;
        static constexpr size_t MAX_QUEUED_BYTES   = 4u << 20;
        static constexpr size_t MAX_ROOTS           = 64;
        static constexpr size_t MAX_ENTRIES         = 4096;
        static constexpr size_t MAX_VISITED         = 65536;
        static constexpr size_t MAX_FILE_BYTES      = 1u << 20;
        static constexpr size_t MAX_PATH_BYTES      = 4096;
        static constexpr size_t MAX_PREFIX_BYTES    = 4096;
        static constexpr size_t MAX_EXTENSIONS      = 16;
        static constexpr size_t MAX_EXTENSION_BYTES = 64;
        static constexpr size_t HEADER_BYTES        = 25; // three 8-digit hex fields + directory bit
        static constexpr size_t READ_BYTES          = 4096;
        static constexpr size_t READS_PER_DISPATCH  = 16;

        // All request values are passed as argv, never interpolated into the
        // script. The helper only writes framed data to stdout; stdout is our
        // CLOEXEC pipe and the helper owns a separate process group.
        static constexpr std::string_view HELPER = R"BASH(LC_ALL=C
export LC_ALL
max_entries=$1
max_visited=$2
max_file_bytes=$3
recursive=$4
executable=$5
directories=$6
name_prefix=$7
extension_count=$8
shift 8
extensions=("${@:1:extension_count}")
shift "$extension_count"

visited=0
emitted=0
stop=0

too_large() {
    local value=$1 limit=$2
    ((${#value} > ${#limit})) || { ((${#value} == ${#limit})) && [[ "$value" > "$limit" ]]; }
}

emit_entry() {
    local root_index=$1 path=$2 bytes=$3 directory=$4 contents=''
    (( ${#path} <= 4096 )) || return
    if (( directory == 0 && max_file_bytes > 0 )); then
        IFS= read -r -N "$bytes" contents < "$path" || true
        # Bash variables cannot represent NUL. Desktop entries are text, so
        # reject a changed or binary file rather than corrupting the frame.
        (( ${#contents} == bytes )) || return
    fi
    printf '%08X%08X%08X%d' "${#path}" "$root_index" "${#contents}" "$directory"
    printf '%s' "$path"
    printf '%s' "$contents"
}

visit_root() {
    local root=$1 root_index=$2 path bytes name name_without_dot extension wanted directory matched
    local -a find_args=(-mindepth 1)
    (( recursive )) || find_args+=(-maxdepth 1)
    while IFS= read -r -d '' path && IFS= read -r -d '' bytes; do
        (( ++visited > max_visited )) && { stop=1; break; }
        [[ "$bytes" =~ ^[0-9]+$ ]] || continue
        if [[ -d "$path" ]]; then
            directory=1
        elif [[ -f "$path" ]]; then
            directory=0
        else
            continue
        fi
        (( directory == 0 || directories != 0 )) || continue
        name=${path##*/}
        [[ -z "$name_prefix" || "$name" == "$name_prefix"* ]] || continue
        if (( extension_count > 0 )); then
            extension=''
            name_without_dot=${name#.}
            [[ "$name_without_dot" == *.* ]] && extension=".${name_without_dot##*.}"
            extension=${extension,,}
            matched=0
            for wanted in "${extensions[@]}"; do
                [[ "$extension" == "$wanted" ]] && { matched=1; break; }
            done
            (( matched )) || continue
        fi
        (( executable == 0 || directory != 0 )) || [[ -x "$path" ]] || continue
        # -f follows symlinks but find's %s reports the LINK length: without a
        # deref'd re-stat, a packaged .desktop symlink truncates its target to
        # that prefix and the app vanishes from every desktop index. (-L:
        # this coreutils does not dereference stat by default.)
        if (( directory == 0 && max_file_bytes > 0 )); then
            bytes=$(stat -L -c %s -- "$path") || continue
            too_large "$bytes" "$max_file_bytes" && continue
        fi
        if emit_entry "$root_index" "$path" "$bytes" "$directory"; then
            (( ++emitted >= max_entries )) && { stop=1; break; }
        fi
    done < <(/usr/bin/find -- "$root" "${find_args[@]}" -printf '%p\0%s\0' 2>/dev/null)
}

root_index=0
for root in "$@"; do
    visit_root "$root" "$root_index"
    (( stop )) && break
    (( ++root_index ))
done
)BASH";

        struct SResult {
            SEntry entry;
            size_t bytes = 0;
        };

        static bool validText(std::string_view value, size_t limit) {
            return value.size() <= limit && value.find('\0') == std::string_view::npos;
        }

        bool prepare(SRequest& request) {
            if (request.maxEntries == 0 || request.roots.empty() || request.roots.size() > MAX_ROOTS || !validText(request.namePrefix, MAX_PREFIX_BYTES) ||
                request.extensions.size() > MAX_EXTENSIONS)
                return false;
            request.maxEntries   = std::min(request.maxEntries, MAX_ENTRIES);
            request.maxFileBytes = std::min(request.maxFileBytes, MAX_FILE_BYTES);
            if (request.maxVisited == 0)
                request.maxVisited = request.maxEntries > MAX_VISITED / 32 ? MAX_VISITED : request.maxEntries * 32;
            request.maxVisited = std::min(request.maxVisited, MAX_VISITED);
            for (auto& root : request.roots)
                if (!root.empty() && (!validText(root.native(), MAX_PATH_BYTES)))
                    return false;
            for (auto& extension : request.extensions) {
                if (!validText(extension, MAX_EXTENSION_BYTES))
                    return false;
                std::ranges::transform(extension, extension.begin(), [](unsigned char character) { return (char)std::tolower(character); });
            }
            return true;
        }

        static bool parseHex(std::string_view text, uint32_t& value) {
            const auto [END, ERROR] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            return ERROR == std::errc{} && END == text.data() + text.size();
        }

        static size_t resultBytes(const SEntry& entry) {
            return entry.path.native().size() + entry.relative.native().size() + entry.contents.size();
        }

        bool spawn(SRequest request) {
            int pipefd[2];
            if (pipe(pipefd) != 0)
                return false;
            const int readFlags = fcntl(pipefd[0], F_GETFL);
            if (readFlags < 0 || fcntl(pipefd[0], F_SETFL, readFlags | O_NONBLOCK) != 0 || fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
                fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                return false;
            }

            std::vector<std::string> args;
            args.reserve(12 + request.extensions.size() + m_roots.size());
            args.emplace_back("/usr/bin/bash");
            args.emplace_back("-c");
            args.emplace_back(HELPER);
            args.emplace_back("hypr-file-index");
            args.push_back(std::to_string(request.maxEntries));
            args.push_back(std::to_string(request.maxVisited));
            args.push_back(std::to_string(request.maxFileBytes));
            args.push_back(request.recursive ? "1" : "0");
            args.push_back(request.executable ? "1" : "0");
            args.push_back(request.directories ? "1" : "0");
            args.push_back(std::move(request.namePrefix));
            args.push_back(std::to_string(request.extensions.size()));
            for (auto& extension : request.extensions)
                args.push_back(std::move(extension));
            for (const auto& root : m_roots)
                args.push_back(root.native());

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            posix_spawn_file_actions_t actions;
            posix_spawnattr_t          attributes;
            if (posix_spawn_file_actions_init(&actions) != 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                return false;
            }
            if (posix_spawnattr_init(&attributes) != 0) {
                posix_spawn_file_actions_destroy(&actions);
                close(pipefd[0]);
                close(pipefd[1]);
                return false;
            }
#ifndef POSIX_SPAWN_SETPGROUP
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            close(pipefd[0]);
            close(pipefd[1]);
            return false; // without a private group we cannot terminate find with the helper
#else
            const bool configured = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO) == 0 &&
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                posix_spawn_file_actions_addclose(&actions, pipefd[0]) == 0 && posix_spawn_file_actions_addclose(&actions, pipefd[1]) == 0 &&
                posix_spawnattr_setpgroup(&attributes, 0) == 0 && posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0;
            if (!configured) {
                posix_spawn_file_actions_destroy(&actions);
                posix_spawnattr_destroy(&attributes);
                close(pipefd[0]);
                close(pipefd[1]);
                return false;
            }
#endif

            pid_t pid = -1;
            const int status = posix_spawn(&pid, "/usr/bin/bash", &actions, &attributes, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            close(pipefd[1]);
            if (status != 0) {
                close(pipefd[0]);
                return false;
            }

            m_fd     = pipefd[0];
            m_pid    = pid;
            m_source = wl_event_loop_add_fd(m_loop, m_fd, WL_EVENT_READABLE, onPipe, this);
            if (m_source)
                return true;

            stopHelper(true);
            return false;
        }

        static int onPipe(int fd, uint32_t mask, void* data) {
            return static_cast<CAsyncFileIndex*>(data)->drain(fd, mask);
        }

        int drain(int fd, uint32_t mask) {
            if (fd != m_fd || m_complete)
                return 0;
            if (mask & WL_EVENT_ERROR) {
                fail();
                return 0;
            }
            if (m_results.size() >= MAX_QUEUED_RESULTS || m_resultBytes >= MAX_QUEUED_BYTES) {
                pause();
                return 0;
            }

            std::array<char, READ_BYTES> bytes;
            for (size_t i = 0; i < READS_PER_DISPATCH; ++i) {
                const ssize_t count = read(fd, bytes.data(), bytes.size());
                if (count > 0) {
                    const size_t MAX_BUFFER = HEADER_BYTES + MAX_PATH_BYTES + m_maxFileBytes + READ_BYTES;
                    if (m_buffer.size() + (size_t)count > MAX_BUFFER) {
                        fail();
                        return 0;
                    }
                    m_buffer.append(bytes.data(), (size_t)count);
                    if (!parseFrames())
                        return 0;
                    if (m_results.size() >= MAX_QUEUED_RESULTS || m_resultBytes >= MAX_QUEUED_BYTES) {
                        pause();
                        return 0;
                    }
                    continue;
                }
                if (count == 0) {
                    m_eof = true;
                    finishIfEof();
                    return 0;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                fail();
                return 0;
            }
            return 0; // yield to timers and input even when the helper floods
        }

        bool parseFrames() {
            size_t consumed = 0;
            while (m_results.size() < MAX_QUEUED_RESULTS && m_resultBytes < MAX_QUEUED_BYTES) {
                const size_t available = m_buffer.size() - consumed;
                if (available < HEADER_BYTES)
                    break;
                const std::string_view header{m_buffer.data() + consumed, HEADER_BYTES};
                uint32_t pathBytes = 0, rootIndex = 0, contentsBytes = 0;
                if (!parseHex(header.substr(0, 8), pathBytes) || !parseHex(header.substr(8, 8), rootIndex) || !parseHex(header.substr(16, 8), contentsBytes) ||
                    (header[24] != '0' && header[24] != '1') || pathBytes == 0 || pathBytes > MAX_PATH_BYTES || rootIndex >= m_roots.size() || contentsBytes > m_maxFileBytes) {
                    fail();
                    return false;
                }
                const size_t frameBytes = HEADER_BYTES + (size_t)pathBytes + (size_t)contentsBytes;
                if (frameBytes > m_buffer.size() - consumed)
                    break;

                const bool DIRECTORY = header[24] == '1';
                if (DIRECTORY && contentsBytes != 0) {
                    fail();
                    return false;
                }
                const std::string_view path{m_buffer.data() + consumed + HEADER_BYTES, pathBytes};
                if (path.find('\0') != std::string_view::npos) {
                    fail();
                    return false;
                }
                SEntry entry{.path = std::filesystem::path{path}, .relative = std::filesystem::path{path}.lexically_relative(m_roots[rootIndex]), .contents = {}, .directory = DIRECTORY};
                if (contentsBytes > 0)
                    entry.contents.assign(m_buffer.data() + consumed + HEADER_BYTES + pathBytes, contentsBytes);
                const size_t bytes = resultBytes(entry);
                if (bytes > MAX_QUEUED_BYTES || m_resultBytes > MAX_QUEUED_BYTES - bytes)
                    break;
                m_resultBytes += bytes;
                m_results.push_back({.entry = std::move(entry), .bytes = bytes});
                consumed += frameBytes;
            }
            if (consumed > 0)
                m_buffer.erase(0, consumed);
            return true;
        }

        void pause() {
            if (m_source && !m_paused) {
                wl_event_source_fd_update(m_source, 0);
                m_paused = true;
            }
        }

        void resumeIfPossible() {
            if (m_source && m_paused && m_results.size() < MAX_QUEUED_RESULTS && m_resultBytes < MAX_QUEUED_BYTES) {
                wl_event_source_fd_update(m_source, WL_EVENT_READABLE);
                m_paused = false;
            }
        }

        void finishIfEof() {
            if (!m_eof || m_complete)
                return;
            if (!parseFrames())
                return;
            if (!m_buffer.empty()) {
                fail();
                return;
            }
            closePipe();
            reapNoWait();
            m_complete = true;
        }

        void clearResults() {
            m_results.clear();
            m_resultBytes = 0;
            m_buffer.clear();
        }

        void closePipe() {
            if (m_source) {
                wl_event_source_remove(m_source);
                m_source = nullptr;
            }
            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
            m_paused = false;
        }

        void reapNoWait() {
            if (m_pid <= 0)
                return;
            for (;;) {
                const pid_t result = waitpid(m_pid, nullptr, WNOHANG);
                if (result < 0 && errno == EINTR)
                    continue;
                break;
            }
            m_pid = -1;
        }

        void stopHelper(bool terminate) {
            closePipe();
            if (m_pid > 0 && terminate)
                kill(-m_pid, SIGTERM); // the Bash worker and its find child share this owned group
            reapNoWait();
            m_eof = false;
        }

        void fail() {
            stopHelper(true);
            clearResults();
            m_complete = true;
        }

        wl_event_loop*                      m_loop         = nullptr;
        wl_event_source*                    m_source       = nullptr;
        int                                 m_fd           = -1;
        pid_t                               m_pid          = -1;
        uint64_t                            m_generation   = 0;
        size_t                              m_maxFileBytes = 0;
        size_t                              m_resultBytes  = 0;
        bool                                m_complete     = true;
        bool                                m_eof          = false;
        bool                                m_paused       = false;
        std::vector<std::filesystem::path> m_roots;
        std::string                         m_buffer;
        std::deque<SResult>                m_results;
    };

} // namespace NHyprCommon
