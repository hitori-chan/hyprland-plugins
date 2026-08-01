// common/fileindex.hpp -- bounded, cancellable file enumeration off compositor paths.
#pragma once

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace NHyprCommon {

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

        void start() {
            if (m_worker.joinable())
                return;
            m_worker = std::jthread([this](std::stop_token stop) { worker(stop); });
        }

        void request(SRequest request) {
            start();
            {
                std::scoped_lock lock(m_mutex);
                m_generation = request.generation;
                m_complete   = 0;
                m_results.clear();
                m_request = std::move(request);
            }
            m_cv.notify_all();
        }

        // A cancellation is another generation, so a slow old filesystem
        // operation cannot publish after the consumer has moved on.
        void cancel(uint64_t generation) {
            {
                std::scoped_lock lock(m_mutex);
                m_generation = generation;
                m_complete   = generation;
                m_results.clear();
                m_request.reset();
            }
            m_cv.notify_all();
        }

        // Move at most max entries onto the event loop. True means this
        // generation is complete and no retained result remains to consume.
        bool poll(uint64_t generation, std::vector<SEntry>& out, size_t max) {
            bool complete = false;
            {
                std::scoped_lock lock(m_mutex);
                while (out.size() < max && !m_results.empty()) {
                    if (m_results.front().generation == generation)
                        out.push_back(std::move(m_results.front().entry));
                    m_results.pop_front();
                }
                complete = m_complete == generation && m_results.empty();
            }
            m_cv.notify_all();
            return complete;
        }

        bool active(uint64_t generation) const {
            std::scoped_lock lock(m_mutex);
            return m_generation == generation && m_complete != generation;
        }

        void exit() {
            if (!m_worker.joinable())
                return;
            m_worker.request_stop();
            m_cv.notify_all();
            m_worker = {};
            std::scoped_lock lock(m_mutex);
            m_request.reset();
            m_results.clear();
            m_generation = m_complete = 0;
        }

      private:
        static constexpr size_t MAX_QUEUED_RESULTS = 128;

        struct SResult {
            uint64_t generation = 0;
            SEntry   entry;
        };

        bool cancelled(uint64_t generation, std::stop_token stop) const {
            if (stop.stop_requested())
                return true;
            std::scoped_lock lock(m_mutex);
            return m_generation != generation;
        }

        bool emit(uint64_t generation, SEntry entry, std::stop_token stop) {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, stop, [this, generation] { return m_generation != generation || m_results.size() < MAX_QUEUED_RESULTS; });
            if (stop.stop_requested() || m_generation != generation)
                return false;
            m_results.push_back({.generation = generation, .entry = std::move(entry)});
            return true;
        }

        static bool matches(const std::filesystem::path& path, const SRequest& request) {
            const auto name = path.filename().string();
            if (!request.namePrefix.empty() && !name.starts_with(request.namePrefix))
                return false;
            if (request.extensions.empty())
                return true;
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char character) { return (char)std::tolower(character); });
            return std::ranges::find(request.extensions, extension) != request.extensions.end();
        }

        static bool readContents(const std::filesystem::path& path, size_t maxBytes, std::string& contents) {
            if (maxBytes == 0)
                return true;
            std::error_code ec;
            const auto      size = std::filesystem::file_size(path, ec);
            if (ec || size > maxBytes || size > std::numeric_limits<size_t>::max())
                return false;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            contents.resize((size_t)size);
            file.read(contents.data(), (std::streamsize)contents.size());
            if (file.gcount() != (std::streamsize)contents.size())
                return false;
            return file.peek() == std::char_traits<char>::eof();
        }

        void enumerate(const SRequest& request, std::stop_token stop) {
            if (request.maxEntries == 0)
                return;
            const size_t visitedCap = request.maxVisited != 0 ? request.maxVisited : request.maxEntries > std::numeric_limits<size_t>::max() / 32 ? std::numeric_limits<size_t>::max() : request.maxEntries * 32;
            size_t       visited    = 0;
            size_t       emitted    = 0;

            for (const auto& root : request.roots) {
                std::error_code ec;
                const auto visit = [&](const std::filesystem::directory_entry& candidate) {
                    if (++visited > visitedCap || cancelled(request.generation, stop))
                        return false;
                    std::error_code entryError;
                    const bool       directory = candidate.is_directory(entryError);
                    if (entryError)
                        return true;
                    if (directory && !request.directories)
                        return true;
                    if (!directory && !candidate.is_regular_file(entryError))
                        return true;
                    if (!matches(candidate.path(), request))
                        return true;
                    if (!directory && request.executable) {
                        const auto status = candidate.status(entryError);
                        if (entryError || (status.permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) == std::filesystem::perms::none)
                            return true;
                    }

                    SEntry entry{.path = candidate.path(), .relative = candidate.path().lexically_relative(root), .directory = directory};
                    if (!directory && !readContents(entry.path, request.maxFileBytes, entry.contents))
                        return true;
                    if (!emit(request.generation, std::move(entry), stop))
                        return false;
                    emitted++;
                    return emitted < request.maxEntries;
                };

                if (request.recursive) {
                    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
                    while (!ec && it != end) {
                        if (!visit(*it))
                            return;
                        it.increment(ec);
                    }
                } else {
                    std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
                    while (!ec && it != end) {
                        if (!visit(*it))
                            return;
                        it.increment(ec);
                    }
                }
                if (emitted >= request.maxEntries || cancelled(request.generation, stop))
                    return;
            }
        }

        void worker(std::stop_token stop) {
            while (!stop.stop_requested()) {
                SRequest request;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, stop, [this] { return m_request.has_value(); });
                    if (stop.stop_requested())
                        return;
                    request = std::move(*m_request);
                    m_request.reset();
                }
                try {
                    enumerate(request, stop);
                } catch (...) {
                    // A hostile filesystem must not escape the worker or
                    // prevent the event loop from accepting later work.
                }
                {
                    std::scoped_lock lock(m_mutex);
                    if (m_generation == request.generation)
                        m_complete = request.generation;
                }
                m_cv.notify_all();
            }
        }

        mutable std::mutex              m_mutex;
        std::condition_variable_any     m_cv;
        std::optional<SRequest>         m_request;
        std::deque<SResult>             m_results;
        std::jthread                    m_worker;
        uint64_t                        m_generation = 0;
        uint64_t                        m_complete   = 0;
    };

} // namespace NHyprCommon
