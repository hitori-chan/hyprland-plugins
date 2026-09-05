#define _GNU_SOURCE
// fixwin — a fixed-size xdg-toplevel (min == max) for placement tests: the
// dialog/splash shape (a Discord-updater-window stand-in).
// usage: fixwin <w> <h> [title]
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
static struct xdg_toplevel *g_top;
static struct wl_buffer    *g_buf;
static int                  g_w = 310, g_h = 360, g_done;
static const char          *g_title = "fixwin";

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
    g_w = argc > 1 ? atoi(argv[1]) : 310;
    g_h = argc > 2 ? atoi(argv[2]) : 360;
    if (g_w < 1)
        g_w = 1;
    if (g_h < 1)
        g_h = 1;
    g_title = argc > 3 ? argv[3] : "fixwin";

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy)
        return 1;

    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_dispatch(g_dpy);
    wl_display_roundtrip(g_dpy);

    g_surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xsd = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    g_top                = xdg_surface_get_toplevel(xsd);
    xdg_wm_base_add_listener(g_wm, &wml, NULL);
    xdg_toplevel_add_listener(g_top, &topl, NULL);
    xdg_toplevel_set_app_id(g_top, g_title);
    xdg_toplevel_set_title(g_top, g_title);
    xdg_toplevel_set_min_size(g_top, g_w, g_h);
    xdg_toplevel_set_max_size(g_top, g_w, g_h); // min == max: the dialog shape

    // the fd lives with the process; one fixture window, no reuse
    const size_t N  = (size_t)g_w * g_h * 4;
    int          fd = memfd_create("fixwin", 0);
    ftruncate(fd, N);
    uint32_t   *px = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (size_t i = 0; i < (size_t)g_w * g_h; i++)
        px[i] = 0xff202030;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, N);
    g_buf                    = wl_shm_pool_create_buffer(pool, 0, g_w, g_h, g_w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_commit(g_surf);

    while (!g_done)
        if (wl_display_dispatch(g_dpy) < 0)
            break;

    xdg_toplevel_destroy(g_top);
    wl_surface_destroy(g_surf);
    wl_buffer_destroy(g_buf);
    xdg_wm_base_destroy(g_wm);
    wl_shm_destroy(g_shm);
    wl_compositor_destroy(g_comp);
    wl_registry_destroy(reg);
    wl_display_disconnect(g_dpy);
    munmap(px, N);
    close(fd);
    return 0;
}
