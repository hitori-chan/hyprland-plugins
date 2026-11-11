// A session lock client (ext-session-lock-v1), as a screen locker is one,
// for the checks that hold input and capture under the lockscreen. Not named
// after any real locker: an idle daemon's "pidof <locker> || lock" must
// never mistake it for the user's own.
//   stdin lines: lock | unlock
//   stdout: locked | finished | unlocked, and what its lock surface hears:
//           kenter | kleave | key <code> | penter | pleave | button <code>
// One dark surface per output. EOF unlocks first, so a run never leaves its
// compositor locked.
#define _GNU_SOURCE
#include <wayland-client.h>
#include "vlock-proto.h"
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_OUTPUTS 8

static struct wl_compositor*                 compositor;
static struct wl_shm*                        shm;
static struct wl_seat*                       seat;
static struct ext_session_lock_manager_v1*   mgr;
static struct wl_output*                     outputs[MAX_OUTPUTS];
static int                                   nOutputs;
static struct ext_session_lock_v1*           lock;
static struct wl_surface*                    surfaces[MAX_OUTPUTS];
static struct ext_session_lock_surface_v1*   lockSurfaces[MAX_OUTPUTS];

static void say(const char* s) {
    printf("%s\n", s);
    fflush(stdout);
}

static void g_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && nOutputs < MAX_OUTPUTS)
        outputs[nOutputs++] = wl_registry_bind(r, name, &wl_output_interface, 1);
}
static void g_rem(void* d, struct wl_registry* r, uint32_t name) {
    (void)d;
    (void)r;
    (void)name;
}
static const struct wl_registry_listener RL = {g_add, g_rem};

// --- what the lock surface hears ---------------------------------------------

static void k_keymap(void* d, struct wl_keyboard* k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d, (void)k, (void)fmt, (void)size;
    close(fd);
}
static void k_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) {
    (void)d, (void)k, (void)serial, (void)s, (void)keys;
    say("kenter");
}
static void k_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) {
    (void)d, (void)k, (void)serial, (void)s;
    say("kleave");
}
static void k_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d, (void)k, (void)serial, (void)time;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        printf("key %u\n", key);
        fflush(stdout);
    }
}
static void k_mods(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void)d, (void)k, (void)serial, (void)a, (void)b, (void)c, (void)g;
}
static void k_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) {
    (void)d, (void)k, (void)rate, (void)delay;
}
static const struct wl_keyboard_listener KL = {k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat};

static void p_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)p, (void)serial, (void)s, (void)x, (void)y;
    say("penter");
}
static void p_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {
    (void)d, (void)p, (void)serial, (void)s;
    say("pleave");
}
static void p_motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)p, (void)time, (void)x, (void)y;
}
static void p_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)d, (void)p, (void)serial, (void)time;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        printf("button %u\n", button);
        fflush(stdout);
    }
}
static void p_axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t v) {
    (void)d, (void)p, (void)time, (void)axis, (void)v;
}
static void p_frame(void* d, struct wl_pointer* p) {
    (void)d, (void)p;
}
static void p_source(void* d, struct wl_pointer* p, uint32_t s) {
    (void)d, (void)p, (void)s;
}
static void p_stop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis) {
    (void)d, (void)p, (void)time, (void)axis;
}
static void p_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t v) {
    (void)d, (void)p, (void)axis, (void)v;
}
static const struct wl_pointer_listener PL = {p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete};

static struct wl_keyboard* keyboard;
static struct wl_pointer*  pointer;

static void s_caps(void* d, struct wl_seat* s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &KL, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &PL, NULL);
    }
}
static void s_name(void* d, struct wl_seat* s, const char* n) {
    (void)d, (void)s, (void)n;
}
static const struct wl_seat_listener SL = {s_caps, s_name};

// --- the lock -------------------------------------------------------------------

static struct wl_buffer* dark(uint32_t w, uint32_t h) {
    const size_t SIZE = (size_t)w * h * 4;
    const int    FD   = memfd_create("vlock", MFD_CLOEXEC);
    if (FD < 0 || ftruncate(FD, (off_t)SIZE) != 0)
        return NULL;
    uint32_t* px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
    if (px == MAP_FAILED) {
        close(FD);
        return NULL;
    }
    for (size_t i = 0; i < SIZE / 4; i++)
        px[i] = 0xff202030;
    munmap(px, SIZE);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, FD, (int32_t)SIZE);
    struct wl_buffer*   buf  = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(FD);
    return buf;
}

static void ls_configure(void* d, struct ext_session_lock_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h) {
    struct wl_surface* s = d;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    struct wl_buffer* buf = dark(w ? w : 1, h ? h : 1);
    if (!buf)
        return;
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_damage(s, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(s);
}
static const struct ext_session_lock_surface_v1_listener LSL = {ls_configure};

static void l_locked(void* d, struct ext_session_lock_v1* l) {
    (void)d, (void)l;
    say("locked");
}
static void l_finished(void* d, struct ext_session_lock_v1* l) {
    (void)d, (void)l;
    say("finished");
}
static const struct ext_session_lock_v1_listener LL = {l_locked, l_finished};

static void doLock(void) {
    if (lock)
        return;
    lock = ext_session_lock_manager_v1_lock(mgr);
    ext_session_lock_v1_add_listener(lock, &LL, NULL);
    for (int i = 0; i < nOutputs; i++) {
        surfaces[i]     = wl_compositor_create_surface(compositor);
        lockSurfaces[i] = ext_session_lock_v1_get_lock_surface(lock, surfaces[i], outputs[i]);
        ext_session_lock_surface_v1_add_listener(lockSurfaces[i], &LSL, surfaces[i]);
    }
}

static void doUnlock(struct wl_display* dpy) {
    if (!lock)
        return;
    for (int i = 0; i < nOutputs; i++) {
        ext_session_lock_surface_v1_destroy(lockSurfaces[i]);
        wl_surface_destroy(surfaces[i]);
    }
    ext_session_lock_v1_unlock_and_destroy(lock);
    lock = NULL;
    wl_display_roundtrip(dpy);
    say("unlocked");
}

int main(void) {
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "vlock: no display\n");
        return 1;
    }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &RL, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr || !compositor || !shm || !nOutputs) {
        fprintf(stderr, "vlock: no ext_session_lock_manager_v1, compositor, shm or output\n");
        return 2;
    }
    if (seat)
        wl_seat_add_listener(seat, &SL, NULL);
    wl_display_roundtrip(dpy);

    struct pollfd fds[2] = {{.fd = STDIN_FILENO, .events = POLLIN}, {.fd = wl_display_get_fd(dpy), .events = POLLIN}};
    char          line[256];
    size_t        have = 0;
    for (;;) {
        while (wl_display_prepare_read(dpy) != 0)
            wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        if (poll(fds, 2, -1) < 0) {
            wl_display_cancel_read(dpy);
            break;
        }
        if (fds[1].revents & POLLIN)
            wl_display_read_events(dpy);
        else
            wl_display_cancel_read(dpy);
        if (wl_display_dispatch_pending(dpy) < 0)
            break;
        if (!(fds[0].revents & (POLLIN | POLLHUP)))
            continue;
        const ssize_t N = read(STDIN_FILENO, line + have, sizeof line - 1 - have);
        if (N <= 0)
            break;
        have += (size_t)N;
        line[have] = 0;
        char* nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (!strcmp(line, "lock"))
                doLock();
            else if (!strcmp(line, "unlock"))
                doUnlock(dpy);
            have -= (size_t)(nl + 1 - line);
            memmove(line, nl + 1, have + 1);
        }
        if (have == sizeof line - 1)
            have = 0; // an overlong line is dropped
    }
    doUnlock(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
