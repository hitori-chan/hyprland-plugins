#define _GNU_SOURCE
// A fixed-size xdg-toplevel (min == max) for placement tests: the
// dialog/splash shape (a Discord-updater-window stand-in). With `drag`, it
// is its own title bar: a press dragged 8 px asks xdg_toplevel.move with
// that press's serial, as a client-side title bar does, and it prints
// `press SERIAL`, `move SERIAL`, `up`.
// usage: fixwin <w> <h> [title] [drag]
#include <fcntl.h>
#include <stdio.h>
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
static struct wl_seat      *g_seat;
static int                  g_w = 310, g_h = 360, g_done, g_drag;
static uint32_t             g_pressSerial;
static int                  g_pressed, g_asked;
static double               g_px, g_py, g_x, g_y;
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
    else if (!strcmp(ifc, "wl_seat") && !g_seat && g_drag)
        g_seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
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

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void) d;
    (void) p;
    (void) serial;
    (void) s;
    g_x = wl_fixed_to_double(x);
    g_y = wl_fixed_to_double(y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void) d;
    (void) p;
    (void) serial;
    (void) s;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void) d;
    (void) p;
    (void) t;
    g_x = wl_fixed_to_double(x);
    g_y = wl_fixed_to_double(y);
    // the title bar's drag: past the threshold, the compositor moves it
    if (g_pressed && !g_asked && ((g_x - g_px) * (g_x - g_px) + (g_y - g_py) * (g_y - g_py)) >= 64) {
        g_asked = 1;
        xdg_toplevel_move(g_top, g_seat, g_pressSerial);
        printf("move %u\n", g_pressSerial);
        fflush(stdout);
    }
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void) d;
    (void) p;
    (void) t;
    (void) b;
    g_pressed = st == WL_POINTER_BUTTON_STATE_PRESSED;
    if (g_pressed) {
        g_pressSerial = serial;
        g_asked       = 0;
        g_px          = g_x;
        g_py          = g_y;
    }
    printf(g_pressed ? "press %u\n" : "up\n", serial);
    fflush(stdout);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void) d;
    (void) p;
    (void) t;
    (void) a;
    (void) v;
}
static const struct wl_pointer_listener ptrl = { .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis };

int main(int argc, char **argv) {
    g_w = argc > 1 ? atoi(argv[1]) : 310;
    g_h = argc > 2 ? atoi(argv[2]) : 360;
    if (g_w < 1)
        g_w = 1;
    if (g_h < 1)
        g_h = 1;
    g_title = argc > 3 ? argv[3] : "fixwin";
    g_drag  = argc > 4 && !strcmp(argv[4], "drag");

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy)
        return 1;

    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_dispatch(g_dpy);
    wl_display_roundtrip(g_dpy);
    if (g_drag && g_seat)
        wl_pointer_add_listener(wl_seat_get_pointer(g_seat), &ptrl, NULL); // v1: the pointer is the seat's for the process's life

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
