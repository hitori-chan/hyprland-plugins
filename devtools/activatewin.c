#define _GNU_SOURCE
// activatewin — the tray-return shape for the map-focus retraction gate:
// a Wayland toplevel that maps (and takes the new-map initial focus),
// then asks for attention through xdg-activation — the exact sequence a
// tray-returning app performs on a new message, and the one the
// compositor cannot distinguish from a user-launched window. With
// focus_on_activate off the ask lands as urgency, and the plugin's
// retraction (windows/retract.hpp) must hand the focus back to the
// pre-arrival window.
//
// Fork protocol (Hyprland/protocols/xdg-activation-v1): the token object
// mints on commit and delivers the token string in its done event; the
// manager's activate request takes the token and the target surface. No
// shared library exposes the wire interfaces, so the fixture carries its
// own copies (interface tables for the proxies, opcodes for the
// opcode-based 1.26 client marshal).
//
// usage: activatewin [hold-s] [app-id]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fixwin-protocol.h"
#include <wayland-client.h>

static struct wl_display    *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm        *g_shm;
static struct xdg_wm_base   *g_wm;
static struct wl_surface    *g_surf;
static struct xdg_surface   *g_xsd;
static struct xdg_toplevel  *g_top;
static struct wl_buffer     *g_buf;
static void                 *g_act; // xdg_activation_v1 (private iface)
static int                   g_done;
static const char           *g_id   = "activatewin";
static int                   g_hold = 10;
static char                  g_tokstr[128];

// ---- xdg-activation-v1, the fork's wire format ----------------------------
// manager: 0 destroy, 1 get_activation_token(n), 2 activate(s, o)
// token:   0 set_serial(u,o), 1 set_app_id(s), 2 set_surface(o),
//          3 commit, 4 destroy | events: 0 done(s)
static const struct wl_interface *xav_dummy[]            = { NULL };
static const struct wl_interface *xav_gettok_types[1]    = { NULL };
static const struct wl_interface *xav_act_types[2]       = { NULL };
static const struct wl_interface *xav_serial_types[2]    = { NULL };
static const struct wl_interface *xav_surf_types[1]      = { NULL };

static const struct wl_message xav_manager_reqs[] = {
    { .name = "destroy", .signature = "", .types = xav_dummy },
    { .name = "get_activation_token", .signature = "n", .types = xav_gettok_types },
    { .name = "activate", .signature = "so", .types = xav_act_types },
};
static const struct wl_interface xav_manager_iface = {
    .name = "xdg_activation_v1", .version = 1,
    .method_count = 3, .methods = xav_manager_reqs,
    .event_count = 0, .events = NULL,
};
static const struct wl_message xav_token_reqs[] = {
    { .name = "set_serial", .signature = "uo", .types = xav_serial_types },
    { .name = "set_app_id", .signature = "s", .types = xav_dummy },
    { .name = "set_surface", .signature = "o", .types = xav_surf_types },
    { .name = "commit", .signature = "", .types = xav_dummy },
    { .name = "destroy", .signature = "", .types = xav_dummy },
};
static const struct wl_message xav_token_events[] = {
    { .name = "done", .signature = "s", .types = xav_dummy },
};
static const struct wl_interface xav_token_iface = {
    .name = "xdg_activation_token_v1", .version = 1,
    .method_count = 5, .methods = xav_token_reqs,
    .event_count = 1, .events = xav_token_events,
};

// ---- registry -------------------------------------------------------------
static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_compositor") && !g_comp)
        g_comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(ifc, "wl_shm") && !g_shm)
        g_shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(ifc, "xdg_wm_base") && !g_wm)
        g_wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(ifc, "xdg_activation_v1") && !g_act)
        g_act = wl_registry_bind(r, id, &xav_manager_iface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void) d;
    (void) r;
    (void) id;
}
static const struct wl_registry_listener regl = { reg_bind, reg_remove };

// ---- xdg shell ------------------------------------------------------------
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

// ---- the tray-return ask ---------------------------------------------------
// the token object's raw dispatcher: its one event is done(token string)
static int tok_dispatch(const void *ud, void *target, uint32_t opcode, const struct wl_message *msg, union wl_argument *args) {
    (void) ud;
    (void) target;
    (void) msg;
    if (opcode != 0)
        return 0;
    snprintf(g_tokstr, sizeof(g_tokstr), "%s", args[0].s);
    // the ask: activate(token, surface) — the surface is the window the ask
    // is about, here our own freshly mapped toplevel
    union wl_argument a[2] = { [0] = { .s = g_tokstr }, [1] = { .o = (void *)g_surf } };
    wl_proxy_marshal_array((struct wl_proxy *)g_act, 2, a);
    fprintf(stderr, "activatewin: sent activation\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1)
        g_hold = atoi(argv[1]);
    if (argc > 2)
        g_id = argv[2];

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy)
        return 1;

    xav_act_types[1]    = &wl_surface_interface;
    xav_serial_types[1] = &wl_seat_interface;
    xav_surf_types[0]   = &wl_surface_interface;

    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_dispatch(g_dpy);
    wl_display_roundtrip(g_dpy);

    if (!g_act) {
        fprintf(stderr, "activatewin: xdg_activation_v1 not offered\n");
        return 1;
    }

    g_surf = wl_compositor_create_surface(g_comp);
    g_xsd  = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    g_top  = xdg_surface_get_toplevel(g_xsd);
    xdg_wm_base_add_listener(g_wm, &wml, NULL);
    xdg_toplevel_add_listener(g_top, &topl, NULL);
    xdg_toplevel_set_app_id(g_top, g_id);
    xdg_toplevel_set_title(g_top, g_id);

    const int W = 400, H = 250, N = W * H * 4;
    int       fd = memfd_create("activatewin", 0);
    ftruncate(fd, N);
    uint32_t *px = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < W * H; i++)
        px[i] = 0xff502080;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, N);
    g_buf                    = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(g_surf, g_buf, 0, 0);
    wl_surface_commit(g_surf); // maps: the compositor takes this as a new window
    wl_display_roundtrip(g_dpy);
    fprintf(stderr, "activatewin: mapped\n");

    // the ask, right after the map — the back-to-back burst the retraction
    // discriminates on (the arrival window). The "n" request is the
    // constructor-style marshal: the new interface is an argument, the id
    // is minted by the library (the 1.26 client API)
    struct wl_proxy *tok = wl_proxy_marshal_flags((struct wl_proxy *)g_act, 1, &xav_token_iface, 1, 0, NULL);
    if (!tok || wl_proxy_add_dispatcher(tok, tok_dispatch, NULL, NULL) != 0)
        return 1;
    wl_proxy_marshal((struct wl_proxy *)tok, 3); // commit -> done(token)

    // hold so the gate can observe the focus return (and the urgent event);
    // dispatch in 500ms chunks so the hold is wall-clock, not event-count
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    int             left = g_hold * 2;
    while (!g_done && left-- > 0)
        if (wl_display_dispatch_timeout(g_dpy, &ts) < 0)
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
