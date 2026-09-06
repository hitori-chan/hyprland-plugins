#define _GNU_SOURCE
// splashwin — the Discord-updater-splash shape for placement tests: a
// CSD toplevel whose committed buffer is larger than the declared
// window geometry (a shadow margin on all sides), min pinned to max
// in the geometry frame.
// usage: splashwin <w> <h> [margin] [app-id] [late] [parented] [resz] [vismargin]
//   w, h   content size = the declared window geometry, min == max (resz: min only)
//   margin shadow inset: the buffer is (w + 2m) x (h + 2m)
//   late     map first (buffer commit), THEN declare the size limits
//   parented also create a big resizable toplevel first and transient-for it
//   resz     declare the min size only: a resizable CSD window with the same offset
//   vismargin paint the margin as opaque light gray (visible over any background)
#include <fcntl.h>
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
static int                  g_w = 300, g_h = 350, g_m = 10, g_late, g_parented, g_resz, g_vis, g_done;
static const char          *g_id = "splashwin";

static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_compositor") && !g_comp)
        g_comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(ifc, "wl_shm") && !g_shm)
        g_shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(ifc, "xdg_wm_base") && !g_wm)
        g_wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
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

    const int BW = g_w + 2 * g_m, BH = g_h + 2 * g_m; // buffer = content + shadow margin

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
        munmap(ppx, PN);
        close(pfd);
    }

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
    const size_t N  = (size_t)BW * BH * 4;
    int          fd = memfd_create("splashwin", 0);
    ftruncate(fd, N);
    uint32_t   *px = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < BH; y++) {
        for (int x = 0; x < BW; x++) {
            const int IN = x >= g_m && x < g_m + g_w && y >= g_m && y < g_m + g_h;
            px[y * BW + x] = IN ? 0xff302020 : (g_vis ? 0xffb0b0b0 : 0x00000000);
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, N);
    g_buf                    = wl_shm_pool_create_buffer(pool, 0, BW, BH, BW * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    if (!g_late) {
        xdg_toplevel_set_min_size(g_top, g_w, g_h);
        if (!g_resz)
            xdg_toplevel_set_max_size(g_top, g_w, g_h); // min == max: the splash shape
    }

    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_commit(g_surf); // maps (late: without any size limits yet)

    if (g_late) {
        // the race shape: the limits land only after the first map commit
        wl_display_roundtrip(g_dpy);
        xdg_toplevel_set_min_size(g_top, g_w, g_h);
        xdg_toplevel_set_max_size(g_top, g_w, g_h);
        wl_surface_commit(g_surf);
    }

    while (!g_done)
        if (wl_display_dispatch(g_dpy) < 0)
            break;

    xdg_toplevel_destroy(g_top);
    wl_surface_destroy(g_surf);
    wl_buffer_destroy(g_buf);
    if (g_parented) {
        xdg_toplevel_destroy(g_ptop);
        wl_surface_destroy(g_psurf);
        wl_buffer_destroy(g_pbuf);
    }
    xdg_wm_base_destroy(g_wm);
    wl_shm_destroy(g_shm);
    wl_compositor_destroy(g_comp);
    wl_registry_destroy(reg);
    wl_display_disconnect(g_dpy);
    munmap(px, N);
    close(fd);
    return 0;
}
