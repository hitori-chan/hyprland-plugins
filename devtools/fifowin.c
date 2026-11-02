#define _GNU_SOURCE
// A FIFO-paced client as Mesa's Vulkan WSI commits (fifo-v1): one content
// update per frame that sets and waits on the barrier. Four SHM buffers
// stand in for the swapchain: an SHM buffer is released when its update is
// applied, so a wait for a free buffer is a wait on the compositor.
// `recreate` replaces the fifo object a third of the way in, as a
// swapchain recreate does, and lets the frames in flight present before
// the new object exists (Mesa's destroy and get_fifo sit a roundtrip
// apart): a barrier must not outlive the object that clears it.
// usage: fifowin fifo|recreate FRAMES
// prints: frames elapsed_ms max_wait_ms presented discarded late
// (late: presents more than 1.5 refreshes apart); exit 1 on a wait over 2 s
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fifo-proto.h"
#include "fixwin-protocol.h"
#include "presentation-proto.h"
#include <wayland-client.h>

#define NBUF 4
#define W 320
#define H 240

static struct wl_display          *g_dpy;
static struct wl_compositor       *g_comp;
static struct wl_shm              *g_shm;
static struct xdg_wm_base         *g_wm;
static struct wp_fifo_manager_v1  *g_fifom;
static struct wp_presentation     *g_pres;
static struct wl_surface          *g_surf;
static struct wp_fifo_v1          *g_fifo;
static int                         g_configured, g_presented, g_discarded;
static double                      g_pt[4096];
static uint32_t                    g_refresh;

struct buf {
    struct wl_buffer *b;
    int               busy;
};
static struct buf g_bufs[NBUF];

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_compositor") && !g_comp)
        g_comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(ifc, "wl_shm") && !g_shm)
        g_shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(ifc, "xdg_wm_base") && !g_wm)
        g_wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(ifc, "wp_fifo_manager_v1") && !g_fifom)
        g_fifom = wl_registry_bind(r, id, &wp_fifo_manager_v1_interface, 1);
    else if (!strcmp(ifc, "wp_presentation") && !g_pres)
        g_pres = wl_registry_bind(r, id, &wp_presentation_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void) d;
    (void) r;
    (void) id;
}
static const struct wl_registry_listener regl = { reg_bind, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) {
    (void) d;
    xdg_wm_base_pong(b, s);
}
static const struct xdg_wm_base_listener wml = { .ping = wm_ping };

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void) d;
    xdg_surface_ack_configure(s, serial);
    g_configured = 1;
}
static const struct xdg_surface_listener xsl = { .configure = xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
    (void) d;
    (void) t;
    (void) w;
    (void) h;
    (void) s;
}
static void top_close(void *d, struct xdg_toplevel *t) {
    (void) d;
    (void) t;
}
static const struct xdg_toplevel_listener topl = { .configure = top_configure, .close = top_close };

static void buf_release(void *d, struct wl_buffer *b) {
    (void) b;
    ((struct buf *) d)->busy = 0;
}
static const struct wl_buffer_listener bufl = { .release = buf_release };

static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) {
    (void) d;
    (void) f;
    (void) o;
}
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t g, uint32_t h, uint32_t k) {
    (void) d;
    (void) e;
    (void) g;
    (void) h;
    (void) k;
    if (g_presented < 4096)
        g_pt[g_presented] = ((double) (((uint64_t) a << 32) | b)) * 1e3 + c / 1e6;
    g_refresh = e;
    g_presented++;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) {
    (void) d;
    g_discarded++;
    wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener fbl = { fb_sync, fb_presented, fb_discarded };

// dispatch until `ready` holds; 0 on a wait longer than timeout_ms
static int wait_for(int (*ready)(void), double timeout_ms, double *waited) {
    double t0 = now_ms();
    while (!ready()) {
        while (wl_display_prepare_read(g_dpy) != 0)
            wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);
        int left = (int) (timeout_ms - (now_ms() - t0));
        if (left <= 0) {
            wl_display_cancel_read(g_dpy);
            return 0;
        }
        struct pollfd p = { wl_display_get_fd(g_dpy), POLLIN, 0 };
        if (poll(&p, 1, left) <= 0) {
            wl_display_cancel_read(g_dpy);
            continue;
        }
        if (wl_display_read_events(g_dpy) < 0 || wl_display_dispatch_pending(g_dpy) < 0) {
            fprintf(stderr, "fifowin: connection lost\n");
            exit(2);
        }
    }
    if (waited)
        *waited = now_ms() - t0;
    return 1;
}
static int configured(void) {
    return g_configured;
}
static int g_committed;
static int all_fed_back(void) {
    return g_presented + g_discarded >= g_committed;
}
static int buf_free(void) {
    for (int i = 0; i < NBUF; i++)
        if (!g_bufs[i].busy)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "fifo") && strcmp(argv[1], "recreate"))) {
        fprintf(stderr, "usage: fifowin fifo|recreate FRAMES\n");
        return 2;
    }
    const int recreate = !strcmp(argv[1], "recreate");
    const int frames   = atoi(argv[2]);

    if (!(g_dpy = wl_display_connect(NULL)))
        return 2;
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_comp || !g_shm || !g_wm || !g_fifom || !g_pres) {
        fprintf(stderr, "fifowin: missing a global (fifo-v1 or presentation-time)\n");
        return 2;
    }
    xdg_wm_base_add_listener(g_wm, &wml, NULL);

    const int stride = W * 4, size = stride * H;
    int fd = memfd_create("fifowin", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t) size * NBUF) < 0)
        return 2;
    uint32_t *px = mmap(NULL, (size_t) size * NBUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED)
        return 2;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, size * NBUF);
    for (int i = 0; i < NBUF; i++) {
        g_bufs[i].b = wl_shm_pool_create_buffer(pool, i * size, W, H, stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(g_bufs[i].b, &bufl, &g_bufs[i]);
    }

    g_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(xs, &xsl, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &topl, NULL);
    xdg_toplevel_set_app_id(top, "fifowin");
    xdg_toplevel_set_title(top, "fifowin");
    wl_surface_commit(g_surf);
    if (!wait_for(configured, 5000, NULL)) {
        fprintf(stderr, "fifowin: no configure\n");
        return 2;
    }
    g_fifo = wp_fifo_manager_v1_get_fifo(g_fifom, g_surf);

    double t0 = now_ms(), maxwait = 0;
    for (int n = 0; n < frames; n++) {
        double waited = 0;
        if (!wait_for(buf_free, 2000, &waited)) {
            printf("%d %.0f stalled %d %d\n", n, now_ms() - t0, g_presented, g_discarded);
            return 1;
        }
        if (n > 0 && waited > maxwait)
            maxwait = waited;
        if (recreate && n == frames / 3) {
            wp_fifo_v1_destroy(g_fifo);
            if (!wait_for(all_fed_back, 2000, NULL)) {
                printf("%d %.0f stalled %d %d\n", n, now_ms() - t0, g_presented, g_discarded);
                return 1;
            }
            g_fifo = wp_fifo_manager_v1_get_fifo(g_fifom, g_surf);
        }
        int i = 0;
        while (g_bufs[i].busy)
            i++;
        uint32_t c = 0xff000000u | (uint32_t) (n * 0x10305);
        for (int p = 0; p < W * H; p++)
            px[i * W * H + p] = c;
        g_bufs[i].busy = 1;
        wl_surface_attach(g_surf, g_bufs[i].b, 0, 0);
        wl_surface_damage_buffer(g_surf, 0, 0, W, H);
        wp_presentation_feedback_add_listener(wp_presentation_feedback(g_pres, g_surf), &fbl, NULL);
        wp_fifo_v1_set_barrier(g_fifo);
        wp_fifo_v1_wait_barrier(g_fifo);
        wl_surface_commit(g_surf);
        g_committed++;
        wl_display_flush(g_dpy);
    }
    // the last frames' feedback
    double end = now_ms();
    wl_display_roundtrip(g_dpy);
    int n = g_presented < 4096 ? g_presented : 4096, slow = 0;
    for (int k = 1; k < n; k++)
        if (g_pt[k] - g_pt[k - 1] > 1.5 * (g_refresh ? g_refresh / 1e6 : 1e3 / 60))
            slow++;
    printf("%d %.0f %.0f %d %d %d\n", frames, end - t0, maxwait, g_presented, g_discarded, slow);
    return 0;
}
