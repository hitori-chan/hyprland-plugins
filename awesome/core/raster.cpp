// awesome/core/raster.cpp — see raster.hpp.
#include "raster.hpp"

#include <hyprland/src/Compositor.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace NAwesome {

    namespace Raster {

        // a cairo surface over bytes it does not own: read in place, never
        // shared with another thread's surface object
        static cairo_surface_t* wrap(const SView& v) {
            return cairo_image_surface_create_for_data(const_cast<uint8_t*>(v.data), v.format, v.w, v.h, v.stride);
        }

        static SPixels copy(const SView& s) {
            SPixels out{.px = std::vector<uint8_t>((size_t)s.w * s.h * 4), .w = s.w, .h = s.h};
            for (int y = 0; y < s.h; y++) {
                uint8_t* row = out.px.data() + (size_t)y * s.w * 4;
                std::memcpy(row, s.data + (size_t)y * s.stride, (size_t)s.w * 4);
                if (s.format == CAIRO_FORMAT_RGB24)
                    for (int x = 0; x < s.w; x++)
                        row[(size_t)x * 4 + 3] = 255; // native-endian ARGB: alpha is the 4th byte on little-endian
            }
            return out;
        }

        // cairo's GOOD filter costs ~6 ns per SOURCE pixel on a large
        // downscale (a 16 MP screenshot: ~90 ms). An integer box average
        // first, to within 2x of the target, costs ~2 and keeps every source
        // pixel's weight; GOOD then does the exact fraction. Premultiplied
        // ARGB32 averages channel-wise; RGB24 gets opaque alpha. Invalid
        // when under 4x there is nothing to gain.
        static SPixels decimate(const SView& s, double tw, double th) {
            const int K = (int)std::min(s.w / (2 * tw), s.h / (2 * th));
            if (K < 2 || (s.format != CAIRO_FORMAT_ARGB32 && s.format != CAIRO_FORMAT_RGB24))
                return {};
            const int             OW = s.w / K, OH = s.h / K;
            SPixels               out{.px = std::vector<uint8_t>((size_t)OW * OH * 4), .w = OW, .h = OH};
            const uint32_t        N = (uint32_t)K * K; // 255 * N fits: K is at most half the shorter side
            std::vector<uint32_t> acc((size_t)OW * 4);
            for (int oy = 0; oy < OH; oy++) {
                std::ranges::fill(acc, 0u);
                for (int y = oy * K; y < (oy + 1) * K; y++) {
                    const uint8_t* p = s.data + (size_t)y * s.stride;
                    for (int ox = 0; ox < OW; ox++) {
                        uint32_t* a = &acc[(size_t)ox * 4];
                        for (int x = 0; x < K; x++, p += 4) {
                            a[0] += p[0];
                            a[1] += p[1];
                            a[2] += p[2];
                            a[3] += p[3];
                        }
                    }
                }
                uint8_t* row = out.px.data() + (size_t)oy * OW * 4;
                for (size_t i = 0; i < acc.size(); i++)
                    row[i] = (uint8_t)(acc[i] / N);
                if (s.format == CAIRO_FORMAT_RGB24)
                    for (int ox = 0; ox < OW; ox++)
                        row[(size_t)ox * 4 + 3] = 255;
            }
            return out;
        }

        // src scaled by (sx, sy) and offset by (ox, oy) into a fresh W x H;
        // a decimated copy keeps the mapping exact per axis (its integer
        // division drops up to K-1 edge pixels)
        static SPixels paint(SView src, int W, int H, double sx, double sy, double ox, double oy) {
            const auto BOX = decimate(src, src.w * sx, src.h * sy);
            if (BOX.valid()) {
                sx *= (double)src.w / BOX.w;
                sy *= (double)src.h / BOX.h;
                src = view(BOX);
            }
            SPixels out{.px = std::vector<uint8_t>((size_t)W * H * 4), .w = W, .h = H};
            auto*   DST = cairo_image_surface_create_for_data(out.px.data(), CAIRO_FORMAT_ARGB32, W, H, W * 4);
            auto*   SRC = wrap(src);
            auto*   CR  = cairo_create(DST);
            cairo_translate(CR, ox, oy);
            cairo_scale(CR, sx, sy);
            cairo_set_source_surface(CR, SRC, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(CR), CAIRO_FILTER_GOOD);
            cairo_paint(CR);
            const bool OK = cairo_status(CR) == CAIRO_STATUS_SUCCESS;
            cairo_destroy(CR);
            cairo_surface_destroy(SRC);
            cairo_surface_flush(DST);
            cairo_surface_destroy(DST);
            return OK ? out : SPixels{};
        }

        SPixels fit(const SView& src, int maxPx) {
            if (!src.data || src.w <= 0 || src.h <= 0 || maxPx <= 0)
                return {};
            if (src.w <= maxPx && src.h <= maxPx)
                return copy(src);
            const double S = std::min((double)maxPx / src.w, (double)maxPx / src.h);
            const int    W = std::max(1, (int)std::lround(src.w * S)), H = std::max(1, (int)std::lround(src.h * S));
            return paint(src, W, H, (double)W / src.w, (double)H / src.h, 0, 0); // whole pixels both ways: no seam
        }

        SPixels cover(const SView& src, int W, int H) {
            if (!src.data || src.w <= 0 || src.h <= 0 || W <= 0 || H <= 0)
                return {};
            const double S = std::max((double)W / src.w, (double)H / src.h);
            return paint(src, W, H, S, S, (W - src.w * S) / 2.0, (H - src.h * S) / 2.0);
        }

        uint64_t hash(const SPixels& p) {
            uint64_t     h = 0x9e3779b97f4a7c15ull ^ ((uint64_t)(uint32_t)p.w << 32 | (uint32_t)p.h);
            const size_t N = p.px.size();
            size_t       i = 0;
            for (; i + 8 <= N; i += 8) {
                uint64_t v;
                std::memcpy(&v, p.px.data() + i, 8);
                h = (h ^ v) * 0x100000001b3ull;
                h ^= h >> 29;
            }
            for (; i < N; i++)
                h = (h ^ p.px[i]) * 0x100000001b3ull;
            return h;
        }

    } // namespace Raster

    namespace {
        // past this many waiting jobs a burst does its work inline: the
        // queue is a smoothing buffer, not a backlog
        constexpr size_t MAX_QUEUED = 32;
    }

    CRasterWorker& CRasterWorker::inst() {
        static CRasterWorker W;
        return W;
    }

    void CRasterWorker::start() {
        if (m_eventFd >= 0 || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        m_eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (m_eventFd < 0)
            return;
        m_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_eventFd, WL_EVENT_READABLE, onReady, this);
        if (!m_source) {
            close(m_eventFd);
            m_eventFd = -1;
            return;
        }
        std::lock_guard lk(m_lock);
        m_stopping = false;
    }

    void CRasterWorker::stop() {
        {
            std::lock_guard lk(m_lock);
            m_stopping = true;
            m_queue.clear(); // their closures go here, on the compositor thread
        }
        if (m_thread.joinable())
            m_thread.join(); // one job's bounded CPU work at most
        std::deque<SJob> dropped;
        {
            std::lock_guard lk(m_lock);
            dropped.swap(m_doneQ);
            m_running = false;
        }
        dropped.clear(); // their dones never run: what they were for is going away
        if (m_source)
            wl_event_source_remove(m_source);
        m_source = nullptr;
        if (m_eventFd >= 0)
            close(m_eventFd);
        m_eventFd = -1;
    }

    bool CRasterWorker::submit(std::function<void()> work, std::function<void()> done) {
        if (m_eventFd < 0)
            return false;
        {
            std::lock_guard lk(m_lock);
            if (m_stopping || m_queue.size() >= MAX_QUEUED)
                return false;
            m_queue.push_back(SJob{std::move(work), std::move(done)});
            if (m_running)
                return true; // the running loop takes it
            m_running = true;
        }
        if (m_thread.joinable())
            m_thread.join(); // one that ran dry: it took nothing after clearing m_running
        try {
            spawn();
        } catch (...) {
            // no thread to be had: the queue held only this job (a thread
            // exits only once it is empty), and the caller does it inline
            std::lock_guard lk(m_lock);
            m_queue.pop_back();
            m_running = false;
            return false;
        }
        return true;
    }

    void CRasterWorker::spawn() {
        // process signals stay with the compositor's threads: the worker
        // inherits a mask blocking all but the faults (a crash in it still
        // reaches the crash handler)
        sigset_t mask, old;
        sigfillset(&mask);
        for (const int SIG : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS, SIGABRT})
            sigdelset(&mask, SIG);
        pthread_sigmask(SIG_SETMASK, &mask, &old);
        try {
            m_thread = std::thread([this]() {
                pthread_setname_np(pthread_self(), "awesome-raster");
                run();
            });
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &old, nullptr);
            throw;
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }

    // A job's work captures only what is safe to touch and release on this
    // thread (its own buffers, atomic shared pointers); the done carries the
    // rest and is only ever run or destroyed on the compositor thread.
    void CRasterWorker::run() {
        for (;;) {
            SJob job;
            {
                std::lock_guard lk(m_lock);
                if (m_queue.empty() || m_stopping) {
                    m_running = false;
                    return;
                }
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            try {
                job.work();
            } catch (...) {} // nothing may escape a thread: std::terminate
            job.work = nullptr;
            {
                std::lock_guard lk(m_lock);
                m_doneQ.push_back(std::move(job));
            }
            const uint64_t ONE = 1;
            [[maybe_unused]] const auto W = write(m_eventFd, &ONE, sizeof ONE);
        }
    }

    int CRasterWorker::onReady(int fd, uint32_t, void* data) {
        uint64_t count = 0;
        [[maybe_unused]] const auto R = read(fd, &count, sizeof count);
        static_cast<CRasterWorker*>(data)->drainDone();
        return 0;
    }

    void CRasterWorker::drainDone() {
        std::deque<SJob> batch;
        {
            std::lock_guard lk(m_lock);
            batch.swap(m_doneQ);
        }
        for (auto& J : batch) {
            try {
                if (J.done)
                    J.done();
            } catch (...) {} // an event-loop callback: nothing may unwind into the loop
        }
    }

} // namespace NAwesome
