// Pixel work off the compositor thread: scale, crop and unpack on bytes
// already in memory.
//
// The compositor thread runs on a realtime budget (rtkit's RLIMIT_RTTIME:
// a CPU burst past 200 ms ends the session), and scaling one screenshot
// for a card is tens of ms of it. A job's work runs on the worker thread
// and touches nothing but its own captures — never a file, a socket, the
// GL context or compositor state — so teardown can always join it within
// one job's bounded time. Its done runs on the event loop (an eventfd
// source); the caller checks that its card is still the one it was for,
// and uploads in its next warm (invariant 4).
//
// The thread starts on demand and exits when the queue runs dry, like the
// state writer's. A full queue (or no thread to be had) refuses the job:
// the caller does the work inline.
#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct wl_event_source;

namespace NAwesome {

    // premultiplied ARGB32 in cairo's layout (BGRA bytes on little-endian),
    // stride w * 4: what createTexture(DRM_FORMAT_ARGB8888) uploads as is
    struct SPixels {
        std::vector<uint8_t> px;
        int                  w = 0, h = 0;

        bool                 valid() const {
            return w > 0 && h > 0 && px.size() >= (size_t)w * h * 4;
        }
    };

    namespace Raster {
        // A source read in place (any thread; the bytes must outlive the
        // call): premultiplied ARGB32 or RGB24 rows of `stride` bytes.
        struct SView {
            const uint8_t* data   = nullptr;
            int            w      = 0, h = 0, stride = 0;
            cairo_format_t format = CAIRO_FORMAT_ARGB32;
        };
        inline SView view(const SPixels& p) {
            return {p.px.data(), p.w, p.h, p.w * 4, CAIRO_FORMAT_ARGB32};
        }

        // Within maxPx x maxPx, aspect kept, never upscaled (a copy when it
        // already fits). Box-decimated first, GOOD for the exact fraction.
        SPixels fit(const SView& src, int maxPx);
        // Exactly W x H, covering it: the overflowing axis center-cropped.
        SPixels cover(const SView& src, int W, int H);
        // A content-free hash of the pixels, a word at a time (a replace
        // carrying the same image-data keeps its texture).
        uint64_t hash(const SPixels& p);
    }

    class CRasterWorker {
      public:
        static CRasterWorker& inst();

        // the supervisor's: before the modules init, after they tear down
        void start();
        void stop(); // joins a running job; queued jobs and their dones are dropped

        // work runs on the worker, done on the event loop after it. false:
        // refused (the queue is full, the worker is stopped) — neither runs.
        bool submit(std::function<void()> work, std::function<void()> done);

      private:
        struct SJob {
            std::function<void()> work, done;
        };

        void spawn();
        void run();
        static int onReady(int fd, uint32_t mask, void* data);
        void       drainDone();

        std::mutex       m_lock;
        std::deque<SJob> m_queue;   // waiting for the worker
        std::deque<SJob> m_doneQ;   // worked: their dones wait for the loop
        bool             m_running  = false;
        bool             m_stopping = false;
        std::thread      m_thread;
        int              m_eventFd = -1;
        wl_event_source* m_source  = nullptr;
    };

} // namespace NAwesome
