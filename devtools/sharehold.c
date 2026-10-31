// Nested-gate screen share: one wlr-screencopy client that keeps
// copying the first output until it is killed, the way a screencast holds
// its session (the compositor keeps one session per client and output).
//
//   sharehold [-d] [-n FRAMES] [INTERVAL_MS]
//
// -d copies with damage, as a screencast does: the compositor holds the
// frame until the screen changes, so a still screen keeps one waiting.
// -n stops asking after FRAMES copied frames and stays connected, as a
// screenshot tool's one shot leaves its session behind.
//
// READY is printed once the first frame is copied: the compositor reports a
// share from its first copied frame. A frame the compositor refuses is
// retried at the same interval (default 100 ms); the first refusal and, at
// exit, the counts go to stderr.
#define _GNU_SOURCE
#include <wayland-client.h>
#include "sharehold-proto.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct zwlr_screencopy_manager_v1* manager;
static struct wl_shm*                     shm;
static struct wl_output*                  output;
static struct wl_buffer*                  buffer;
static uint32_t                           bufW, bufH, bufStride, bufFormat;
static struct zwlr_screencopy_frame_v1*   frame;
static int                                interval_ms = 100;
static int                                withDamage;
static long                               maxFrames; // 0: no end
static int64_t                            next_at;
static long                               copied, refused;
static volatile sig_atomic_t              stopping;

static int64_t monotonicMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void onSignal(int sig) {
    (void)sig;
    stopping = 1;
}

static void registryAdd(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
    else if (!strcmp(interface, wl_shm_interface.name))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, wl_output_interface.name) && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, 1);
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registryListener = {registryAdd, registryRemove};

// one shm buffer, remade only when the frame's shape changes
static int ensureBuffer(uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    if (buffer && format == bufFormat && w == bufW && h == bufH && stride == bufStride)
        return 1;
    if (buffer)
        wl_buffer_destroy(buffer);
    buffer         = NULL;
    const size_t SIZE = (size_t)stride * h;
    const int    FD   = memfd_create("sharehold", MFD_CLOEXEC);
    if (FD < 0)
        return 0;
    if (ftruncate(FD, (off_t)SIZE) < 0) {
        close(FD);
        return 0;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, FD, (int32_t)SIZE);
    buffer                   = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, format);
    wl_shm_pool_destroy(pool);
    close(FD);
    bufFormat = format;
    bufW      = w;
    bufH      = h;
    bufStride = stride;
    return 1;
}

static void frameBuffer(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    (void)data;
    (void)f;
    ensureBuffer(format, w, h, stride);
}

static void frameFlags(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t flags) {
    (void)data;
    (void)f;
    (void)flags;
}

static void frameEnd(int ok) {
    zwlr_screencopy_frame_v1_destroy(frame);
    frame   = NULL;
    next_at = monotonicMs() + interval_ms;
    if (ok && copied++ == 0) {
        puts("READY");
        fflush(stdout);
    }
    if (!ok && refused++ == 0)
        fputs("sharehold: the compositor refused a frame (retrying)\n", stderr);
}

static void frameReady(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t hi, uint32_t lo, uint32_t nsec) {
    (void)data;
    (void)f;
    (void)hi;
    (void)lo;
    (void)nsec;
    frameEnd(1);
}

static void frameFailed(void* data, struct zwlr_screencopy_frame_v1* f) {
    (void)data;
    (void)f;
    frameEnd(0);
}

static void frameDamage(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)data;
    (void)f;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

static void frameDmabuf(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t format, uint32_t w, uint32_t h) {
    (void)data;
    (void)f;
    (void)format;
    (void)w;
    (void)h;
}

// every buffer type is announced: copy into the shm one
static void frameBufferDone(void* data, struct zwlr_screencopy_frame_v1* f) {
    (void)data;
    if (buffer && withDamage)
        zwlr_screencopy_frame_v1_copy_with_damage(f, buffer);
    else if (buffer)
        zwlr_screencopy_frame_v1_copy(f, buffer);
    else
        frameEnd(0);
}

static const struct zwlr_screencopy_frame_v1_listener frameListener = {frameBuffer, frameFlags, frameReady, frameFailed, frameDamage, frameDmabuf, frameBufferDone};

int main(int argc, char** argv) {
    for (int opt; (opt = getopt(argc, argv, "dn:")) != -1;) {
        if (opt == 'd')
            withDamage = 1;
        else if (opt == 'n' && atol(optarg) > 0)
            maxFrames = atol(optarg);
        else {
            fprintf(stderr, "usage: sharehold [-d] [-n FRAMES] [INTERVAL_MS]\n");
            return 2;
        }
    }
    if (argc - optind > 1) {
        fprintf(stderr, "usage: sharehold [-d] [-n FRAMES] [INTERVAL_MS]\n");
        return 2;
    }
    if (argc - optind == 1)
        interval_ms = atoi(argv[optind]) > 0 ? atoi(argv[optind]) : 100;
    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 3;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager || !shm || !output)
        return 4;
    if (zwlr_screencopy_manager_v1_get_version(manager) < 3)
        return 5; // buffer_done is what says every buffer type is in

    const int displayFd = wl_display_get_fd(display);
    while (!stopping) {
        const int DONE = maxFrames && copied >= maxFrames; // connected, asking no more
        if (!frame && !DONE && monotonicMs() >= next_at) {
            frame = zwlr_screencopy_manager_v1_capture_output(manager, 0, output);
            zwlr_screencopy_frame_v1_add_listener(frame, &frameListener, NULL);
        }
        wl_display_flush(display);
        struct pollfd pfd     = {.fd = displayFd, .events = POLLIN};
        int64_t       left    = frame ? 1000 : next_at - monotonicMs();
        const int     timeout = !frame && DONE ? -1 : left <= 0 ? 0 : (int)left;
        const int     ready   = poll(&pfd, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && (pfd.revents & (POLLERR | POLLHUP))))
            break;
        if (ready > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(display) < 0)
            break;
    }

    fprintf(stderr, "sharehold: %ld copied, %ld refused\n", copied, refused);
    if (frame)
        zwlr_screencopy_frame_v1_destroy(frame);
    if (buffer)
        wl_buffer_destroy(buffer);
    zwlr_screencopy_manager_v1_destroy(manager);
    wl_output_destroy(output);
    wl_shm_destroy(shm);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
