#define _GNU_SOURCE
// A client born fullscreen (a game, a benchmark, a video player's --fs):
// asks for fullscreen before its initial commit, then maps at whatever
// size the compositor configures and follows every later configure.
// Prints one line per xdg_toplevel configure, `W H STATE...`, and `map`
// once its first buffer is committed, until killed.
// usage: fsask APP_ID
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fixwin-protocol.h"
#include <wayland-client.h>

static struct wl_compositor *g_comp;
static struct wl_shm        *g_shm;
static struct xdg_wm_base   *g_wm;
static int                   g_w, g_h, g_configured;
static uint32_t              g_serial;

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

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void) d;
    (void) s;
    g_serial     = serial;
    g_configured = 1;
}
static const struct xdg_surface_listener xsl = { .configure = xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void) d;
    (void) t;
    printf("%d %d", w, h);
    uint32_t *s;
    wl_array_for_each(s, states) printf(" %u", *s);
    printf("\n");
    fflush(stdout);
    g_w = w > 0 ? w : 320;
    g_h = h > 0 ? h : 240;
}
static void top_close(void *d, struct xdg_toplevel *t) {
    (void) d;
    (void) t;
}
static const struct xdg_toplevel_listener topl = { .configure = top_configure, .close = top_close };

static void buf_release(void *d, struct wl_buffer *b) {
    (void) d;
    wl_buffer_destroy(b);
}
static const struct wl_buffer_listener bufl = { .release = buf_release };

static struct wl_buffer *solid(int w, int h) {
    int fd = memfd_create("fsask", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t) w * h * 4) < 0)
        return NULL;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, w * h * 4);
    struct wl_buffer   *b    = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &bufl, NULL);
    return b;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fsask APP_ID\n");
        return 2;
    }
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy)
        return 2;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_roundtrip(dpy);
    if (!g_comp || !g_shm || !g_wm)
        return 2;
    xdg_wm_base_add_listener(g_wm, &wml, NULL);

    struct wl_surface   *surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface  *xs   = xdg_wm_base_get_xdg_surface(g_wm, surf);
    struct xdg_toplevel *top  = xdg_surface_get_toplevel(xs);
    xdg_surface_add_listener(xs, &xsl, NULL);
    xdg_toplevel_add_listener(top, &topl, NULL);
    xdg_toplevel_set_app_id(top, argv[1]);
    xdg_toplevel_set_fullscreen(top, NULL);
    wl_surface_commit(surf);

    int mapped = 0;
    for (;;) {
        if (g_configured) {
            g_configured = 0;
            xdg_surface_ack_configure(xs, g_serial);
            struct wl_buffer *b = solid(g_w, g_h);
            if (!b)
                return 2;
            wl_surface_attach(surf, b, 0, 0);
            wl_surface_damage_buffer(surf, 0, 0, g_w, g_h);
            wl_surface_commit(surf);
            if (!mapped) {
                mapped = 1;
                printf("map\n");
                fflush(stdout);
            }
        }
        if (wl_display_dispatch(dpy) < 0)
            return 1;
    }
}
