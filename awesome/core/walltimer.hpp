// awesome/core/walltimer.hpp — a deadline on the WALL clock.
//
// The event loop's timers are CLOCK_MONOTONIC, which stands still through
// a suspend: a deadline that is a time of day (the minute the clock shows,
// the end of a timed Do Not Disturb) fired as late as the laptop slept. An
// absolute CLOCK_REALTIME timerfd fires at once on resume, and
// CANCEL_ON_SET wakes it on a clock step (NTP, a manual set): the callback
// runs then too, and the owner re-arms against the new time.
#pragma once

#include "hop.hpp"

#include <hyprland/src/Compositor.hpp>

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <functional>

namespace NAwesome {

    class CWallTimer {
      public:
        CWallTimer() = default;
        ~CWallTimer() {
            stop();
        }
        CWallTimer(const CWallTimer&)            = delete;
        CWallTimer& operator=(const CWallTimer&) = delete;

        // the fd and its event source; false: no timer to be had (the
        // owner's deadline then never fires)
        bool start(std::function<void()> fn) {
            stop();
            m_fn = std::move(fn);
            m_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
            if (m_fd < 0)
                return false;
            m_src = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_fd, WL_EVENT_READABLE, onFd, this);
            return m_src != nullptr;
        }

        // fire at `unixSeconds` (one already past fires at once); 0 disarms
        void arm(int64_t unixSeconds) {
            if (m_fd < 0)
                return;
            itimerspec its{};
            its.it_value.tv_sec = (time_t)unixSeconds;
            timerfd_settime(m_fd, unixSeconds > 0 ? TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET : 0, &its, nullptr);
        }

        void stop() {
            if (m_src)
                wl_event_source_remove(m_src); // the source before the fd it watches
            m_src = nullptr;
            if (m_fd >= 0)
                close(m_fd);
            m_fd = -1;
            m_fn = nullptr;
        }

      private:
        static int onFd(int fd, uint32_t, void* data) {
            uint64_t expirations = 0;
            if (read(fd, &expirations, sizeof expirations) < 0 && errno != ECANCELED)
                return 0; // EAGAIN: a re-arm raced the wakeup
            auto* self = static_cast<CWallTimer*>(data);
            if (tearingDown() || !self->m_fn)
                return 0;
            // the fd dispatch is libwayland's C: nothing may unwind into it
            try {
                self->m_fn();
            } catch (...) {}
            return 0;
        }

        int                   m_fd  = -1;
        wl_event_source*      m_src = nullptr;
        std::function<void()> m_fn;
    };

} // namespace NAwesome
