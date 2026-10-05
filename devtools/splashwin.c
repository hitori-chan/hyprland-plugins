#define _GNU_SOURCE
// splashwin — the Discord-updater-splash shape for placement tests: a
// CSD toplevel whose committed buffer is larger than the declared
// window geometry (a shadow margin on all sides), min pinned to max
// in the geometry frame.
// usage: splashwin <w> <h> [margin] [app-id] [late] [parented] [resz] [vismargin] [pinx] [parentonly] [pgeo] [follow] [unmaxwhenmaxed]
//   w, h   content size = the declared window geometry, min == max (resz: min only)
//   margin shadow inset: the buffer is (w + 2m) x (h + 2m)
//   late     map first (buffer commit), THEN declare the size limits
//   parented also create a big resizable toplevel first and transient-for it
//   resz     declare the min size only: a resizable CSD window with the same offset
//   vismargin paint the margin as opaque light gray (visible over any background)
//   pinx     pin the width only (max = w x 0): the per-axis pin shape
//   parentonly create only the parent toplevel (no child)
//   pgeo     give the parent an explicit window geometry (0,0,PW,PH)
//   follow   like a real CSD client: resize the content to the configured
//            size (content = the configure, buffer = content + 2m) and
//            recommit — the shape that exposes the box/content frame
//            mismatch (the content-clip the CSD content-frame fix removes)
//   unmaxwhenmaxed  once a configure tells it maximized, ask to unmaximize
//            1.5 s later — the CSD titlebar restore button (Firefox's
//            double-click) on a compositor/plugin-maximized window
// SPLASHWIN_POINTER_LOG=<path>: append "at X Y" (surface-local, the buffer
//   origin = the margin's outer corner) on every pointer enter/motion over
//   the window — where a click would land in the client's own coordinates
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fixwin-protocol.h"
#include <wayland-client.h>

static struct wl_display   *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm       *g_shm;
static struct xdg_wm_base  *g_wm;
static struct wl_surface   *g_surf;
static struct xdg_surface  *g_xsd;
static struct xdg_toplevel *g_top;
static struct wl_buffer    *g_buf;
static struct wl_surface   *g_psurf;
static struct xdg_surface  *g_pxsd;
static struct xdg_toplevel *g_ptop;
static struct wl_buffer    *g_pbuf;
static struct wl_seat      *g_seat;
static struct wl_pointer   *g_ptr;
static FILE                *g_ptrlog;
static int                  g_w = 300, g_h = 350, g_m = 10, g_late, g_parented, g_resz, g_vis, g_pinx, g_parentonly, g_pgeo, g_follow, g_unmaxwhenmaxed, g_done;
static long long           g_unmaxAt; // monotonic ms; 0 = nothing scheduled
static const char          *g_id = "splashwin";

// the pointer log: v1 pointer events, ours only
static void ptr_at(struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    if (s != g_surf)
        return;
    fprintf(g_ptrlog, "at %.0f %.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
    fflush(g_ptrlog);
}
static struct wl_surface *g_ptrSurf;
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void) d;
    (void) p;
    (void) serial;
    g_ptrSurf = s;
    ptr_at(s, x, y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void) d;
    (void) p;
    (void) serial;
    (void) s;
    g_ptrSurf = NULL;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void) d;
    (void) p;
    (void) t;
    ptr_at(g_ptrSurf, x, y);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void) d;
    (void) p;
    (void) serial;
    (void) t;
    (void) b;
    (void) st;
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void) d;
    (void) p;
    (void) t;
    (void) a;
    (void) v;
}
static const struct wl_pointer_listener ptrl = { .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis };

// the gate's virtual pointer exists only while a gesture runs: bind on
// every capability gain, release on the loss
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void) d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(g_ptr, &ptrl, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g_ptr) {
        wl_pointer_destroy(g_ptr); // v1 has no release request
        g_ptr     = NULL;
        g_ptrSurf = NULL;
    }
}
static const struct wl_seat_listener seatl = { .capabilities = seat_caps };

static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_compositor") && !g_comp)
        g_comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(ifc, "wl_shm") && !g_shm)
        g_shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(ifc, "xdg_wm_base") && !g_wm)
        g_wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(ifc, "wl_seat") && !g_seat && g_ptrlog) {
        g_seat = wl_registry_bind(r, id, &wl_seat_interface, 1); // v1: enter/leave/motion/button/axis only
        wl_seat_add_listener(g_seat, &seatl, NULL);              // before its first capabilities event
    }
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

static void buf_release(void *d, struct wl_buffer *b) {
    (void) d;
    wl_buffer_destroy(b); // the compositor is done with it: safe to destroy
}
static const struct wl_buffer_listener bufl = { .release = buf_release };

// One buffer per paint; a committed buffer stays alive until its release
// event (destroying it earlier is a protocol error — the compositor may
// still be reading it). Buffer creation only: the caller decides when the
// attach+commit happens. For the initial paint that MUST be after the
// toplevel's min/max size requests, or the window maps without the
// fixed-size hints and the layout tiles it (the splash float heuristic
// reads the hints at map).
static void makeContent(int W, int H) {
    const int BW = W + 2 * g_m, BH = H + 2 * g_m;
    const size_t N = (size_t)BW * BH * 4;
    int          fd = memfd_create("splashwin", 0);
    ftruncate(fd, N);
    uint32_t   *px = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < BH; y++) {
        for (int x = 0; x < BW; x++) {
            const int IN = x >= g_m && x < g_m + W && y >= g_m && y < g_m + H;
            px[y * BW + x] = IN ? 0xff302020 : (g_vis ? 0xffb0b0b0 : 0x00000000);
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, N);
    g_buf = wl_shm_pool_create_buffer(pool, 0, BW, BH, BW * 4, WL_SHM_FORMAT_ARGB8888);
    // the release listener owns destruction once the compositor is done
    wl_buffer_add_listener(g_buf, &bufl, NULL);
    wl_shm_pool_destroy(pool);
    g_w = W;
    g_h = H;
    // the fd lives with the process; the buffer is the process's
    munmap(px, N);
    close(fd);
}

static void sendContent(void) {
    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_commit(g_surf);
}

static long long nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
    (void) d;
    (void) t;
    if (g_unmaxwhenmaxed) {
        const uint32_t *st;
        wl_array_for_each(st, s) {
            if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED && !g_unmaxAt) {
                g_unmaxAt        = nowMs() + 1500;
                g_unmaxwhenmaxed = 0; // one restore per run
            }
        }
    }
    if (!g_follow || w < 1 || h < 1)
        return; // 0x0 = "you decide": keep the own size
    if (w == g_w && h == g_h)
        return; // already there: no commit storm
    makeContent(w, h);
    sendContent();
}
static void top_close(void *d, struct xdg_toplevel *t) {
    (void) d;
    (void) t;
    g_done = 1;
}
static const struct xdg_toplevel_listener topl = { .configure = top_configure, .close = top_close };

int main(int argc, char **argv) {
    g_w  = argc > 1 ? atoi(argv[1]) : 300;
    g_h  = argc > 2 ? atoi(argv[2]) : 350;
    g_m  = argc > 3 ? atoi(argv[3]) : 10;
    if (g_w < 1)
        g_w = 1;
    if (g_h < 1)
        g_h = 1;
    if (g_m < 0)
        g_m = 0;
    if (argc > 4)
        g_id   = argv[4];
    g_late = argc > 5 && !strcmp(argv[5], "late");
    g_parented = argc > 6 && !strcmp(argv[6], "parented");
    g_resz = argc > 7 && !strcmp(argv[7], "resz");
    g_vis  = argc > 8 && !strcmp(argv[8], "vismargin");
    g_pinx = argc > 9 && !strcmp(argv[9], "pinx");
    g_parentonly = argc > 10 && !strcmp(argv[10], "parentonly");
    g_pgeo = argc > 11 && !strcmp(argv[11], "pgeo");
    g_follow = argc > 12 && !strcmp(argv[12], "follow");
    g_unmaxwhenmaxed = argc > 13 && !strcmp(argv[13], "unmaxwhenmaxed");
    if (getenv("SPLASHWIN_POINTER_LOG"))
        g_ptrlog = fopen(getenv("SPLASHWIN_POINTER_LOG"), "a");

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy)
        return 1;

    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_dispatch(g_dpy);
    wl_display_roundtrip(g_dpy);

    if (g_parented) {
        // the main window: big, resizable, same class — covers the center
        g_psurf = wl_compositor_create_surface(g_comp);
        g_pxsd  = xdg_wm_base_get_xdg_surface(g_wm, g_psurf);
        g_ptop  = xdg_surface_get_toplevel(g_pxsd);
        xdg_toplevel_add_listener(g_ptop, &topl, NULL);
        xdg_toplevel_set_app_id(g_ptop, "discord");
        xdg_toplevel_set_title(g_ptop, "Discord");
        const int PW = 1000, PH = 600;
        const size_t PN = (size_t)PW * PH * 4;
        int          pfd = memfd_create("splashwin-parent", 0);
        ftruncate(pfd, PN);
        uint32_t   *ppx = mmap(NULL, PN, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
        for (size_t i = 0; i < (size_t)PW * PH; i++)
            ppx[i] = 0xff203040;
        struct wl_shm_pool *ppool = wl_shm_create_pool(g_shm, pfd, PN);
        g_pbuf                    = wl_shm_pool_create_buffer(ppool, 0, PW, PH, PW * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(ppool);
        wl_surface_attach(g_psurf, g_pbuf, 0, 0);
        wl_surface_commit(g_psurf);
        wl_display_roundtrip(g_dpy);
        // like the child's buffer: the fd lives with the process; closing it
        // while the compositor is still mapping the buffer destroys the surface
        if (g_pgeo)
            xdg_surface_set_window_geometry(g_pxsd, 0, 0, PW, PH);
    }

    if (g_parentonly)
        goto runloop;

    g_surf = wl_compositor_create_surface(g_comp);
    g_xsd  = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    g_top  = xdg_surface_get_toplevel(g_xsd);
    xdg_wm_base_add_listener(g_wm, &wml, NULL);
    xdg_toplevel_add_listener(g_top, &topl, NULL);
    xdg_toplevel_set_app_id(g_top, g_id);
    xdg_toplevel_set_title(g_top, g_id);
    if (g_parented)
        xdg_toplevel_set_parent(g_top, g_ptop);
    // the CSD content frame: inset from the surface on all sides
    xdg_surface_set_window_geometry(g_xsd, g_m, g_m, g_w, g_h);

    // the fd lives with the process; one fixture window, no reuse
    makeContent(g_w, g_h);

    if (!g_late) {
        xdg_toplevel_set_min_size(g_top, g_w, g_h);
        if (g_pinx)
            xdg_toplevel_set_max_size(g_top, g_w, 0); // width pinned, height free
        else if (!g_resz)
            xdg_toplevel_set_max_size(g_top, g_w, g_h); // min == max: the splash shape
    }

    sendContent(); // maps (late: without any size limits yet)

    if (g_late) {
        // the race shape: the limits land only after the first map commit
        wl_display_roundtrip(g_dpy);
        xdg_toplevel_set_min_size(g_top, g_w, g_h);
        xdg_toplevel_set_max_size(g_top, g_w, g_h);
        sendContent();
    }

runloop:
    while (!g_done) {
        // a scheduled unmaximize needs a timed wait; otherwise block
        wl_display_flush(g_dpy);
        struct pollfd pfd = {.fd = wl_display_get_fd(g_dpy), .events = POLLIN};
        const int     TIMEOUT = g_unmaxAt ? (int)(g_unmaxAt > nowMs() ? g_unmaxAt - nowMs() : 0) : -1;
        if (poll(&pfd, 1, TIMEOUT) > 0 && wl_display_dispatch(g_dpy) < 0)
            break;
        if (g_unmaxAt && nowMs() >= g_unmaxAt) {
            g_unmaxAt = 0;
            if (g_top)
                xdg_toplevel_unset_maximized(g_top);
        }
    }

    // a real CSD client's teardown order (GTK, Firefox): the toplevel
    // role, then the xdg_surface — whose destroy is what unmaps the window,
    // with the toplevel already gone — then the wl_surface
    if (g_top) { // parentonly maps no child
        xdg_toplevel_destroy(g_top);
        xdg_surface_destroy(g_xsd);
        wl_surface_destroy(g_surf);
        // deliver the teardown in this order while still connected, like an
        // app that outlives its window: wl_display_disconnect does not
        // flush, and a bare disconnect lets the compositor reap the objects
        // in its own order (the toplevel then outlives the unmap and masks
        // the close-order class this fixture exists to reproduce)
        wl_display_roundtrip(g_dpy);
    }
    // buffers already released by the compositor are gone; the last
    // attached one (if any) may still be referenced — the process exit
    // reclaims it, so destroy only if the release listener hasn't run.
    // (wl_buffer_destroy after release would double-free.)
    if (g_parented) {
        xdg_toplevel_destroy(g_ptop);
        xdg_surface_destroy(g_pxsd);
        wl_surface_destroy(g_psurf);
        wl_buffer_destroy(g_pbuf);
    }
    xdg_wm_base_destroy(g_wm);
    wl_shm_destroy(g_shm);
    wl_compositor_destroy(g_comp);
    wl_registry_destroy(reg);
    wl_display_disconnect(g_dpy);
    return 0;
}
