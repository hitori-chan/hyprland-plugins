// common/clipboard.hpp — bounded asynchronous reads from the current Wayland
// clipboard selection. A compositor-drawn editor has no wl_data_device of its
// own, so it asks the seat's public IDataSource directly, then drains the pipe
// from the Wayland event loop. No read blocks compositor dispatch.
#pragma once

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <hyprutils/os/FileDescriptor.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <string>
#include <unistd.h>
#include <utility>

namespace NHyprCommon {

    class CClipboardRead {
      public:
        CClipboardRead() = default;
        CClipboardRead(const CClipboardRead&) = delete;
        CClipboardRead& operator=(const CClipboardRead&) = delete;

        ~CClipboardRead() {
            cancel();
        }

        bool request(size_t maxBytes, std::function<void(std::string)> done) {
            cancel();
            if (maxBytes == 0 || !done || !g_pSeatManager || !g_pCompositor || !g_pCompositor->m_wlEventLoop || !g_pEventLoopManager)
                return false;

            const auto SOURCE = g_pSeatManager->m_selection.currentSelection.lock();
            if (!SOURCE)
                return false;

            static constexpr const char* PREFERRED[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING"};
            const auto MIMES = SOURCE->mimes();
            const char* MIME = nullptr;
            for (const auto* CANDIDATE : PREFERRED)
                if (std::ranges::find(MIMES, CANDIDATE) != MIMES.end()) {
                    MIME = CANDIDATE;
                    break;
                }
            if (!MIME)
                return false;

            int fds[2] = {-1, -1};
            if (pipe2(fds, O_CLOEXEC) != 0)
                return false;
            const int FLAGS = fcntl(fds[0], F_GETFL, 0);
            if (FLAGS < 0 || fcntl(fds[0], F_SETFL, FLAGS | O_NONBLOCK) != 0) {
                close(fds[0]);
                close(fds[1]);
                return false;
            }

            m_fd       = fds[0];
            m_maxBytes = maxBytes;
            m_done     = std::move(done);
            m_data.clear();
            m_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, readable, this);
            if (!m_source) {
                close(fds[1]);
                cancel();
                return false;
            }

            m_timeout = makeShared<CEventLoopTimer>(std::chrono::milliseconds(1500), [](SP<CEventLoopTimer>, void* data) { static_cast<CClipboardRead*>(data)->cancel(); }, this);
            g_pEventLoopManager->addTimer(m_timeout);

            Hyprutils::OS::CFileDescriptor WRITE{fds[1]};
            try {
                SOURCE->accepted(MIME);
                SOURCE->send(MIME, std::move(WRITE));
            } catch (...) {
                cancel();
                return false;
            }
            return true;
        }

        void cancel() {
            if (m_timeout && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(m_timeout);
            m_timeout.reset();
            if (m_source)
                wl_event_source_remove(m_source);
            m_source = nullptr;
            if (m_fd >= 0)
                close(m_fd);
            m_fd = -1;
            m_data.clear();
            m_done = {};
            m_maxBytes = 0;
        }

      private:
        static int readable(int, uint32_t mask, void* data) {
            return static_cast<CClipboardRead*>(data)->onReadable(mask);
        }

        int onReadable(uint32_t mask) {
            bool finished = false;
            for (;;) {
                char  buf[512];
                const auto N = read(m_fd, buf, std::min(sizeof(buf), m_maxBytes - m_data.size()));
                if (N > 0) {
                    m_data.append(buf, (size_t)N);
                    if (m_data.size() >= m_maxBytes) {
                        finished = true;
                        break;
                    }
                    continue;
                }
                if (N == 0) {
                    finished = true;
                    break;
                }
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    finished = true;
                break;
            }
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
                finished = true;
            if (finished)
                finish();
            return 0;
        }

        void finish() {
            auto DATA = std::move(m_data);
            auto DONE = std::move(m_done);
            if (m_timeout && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(m_timeout);
            m_timeout.reset();
            if (m_source)
                wl_event_source_remove(m_source);
            m_source = nullptr;
            if (m_fd >= 0)
                close(m_fd);
            m_fd = -1;
            m_maxBytes = 0;
            if (DONE)
                DONE(std::move(DATA));
        }

        int                              m_fd = -1;
        wl_event_source*                 m_source = nullptr;
        SP<CEventLoopTimer>              m_timeout;
        size_t                           m_maxBytes = 0;
        std::string                      m_data;
        std::function<void(std::string)> m_done;
    };

} // namespace NHyprCommon
