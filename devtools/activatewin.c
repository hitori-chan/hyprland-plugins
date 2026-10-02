#define _GNU_SOURCE
// activatewin — the xdg-activation fixture for the focus gate: a Wayland
// toplevel that maps (taking the new-map initial focus), then asks for
// activation through xdg-activation — the exact sequence a tray-returning
// app performs on a new message. With an optional delay the ask lands
// after the gate moved the compositor away, so it arrives at a
// NOT-VISIBLE window (awesome's isvisible/urgent branch: urgency, no
// focus, no workspace switch).
//
// Fork protocol (Hyprland/protocols/xdg-activation-v1): the token object
// mints on commit and delivers the token string in its done event; the
// manager's activate request takes the token and the target surface. No
// shared library exposes the wire interfaces, so the fixture carries its
// own copies (interface tables for the proxies, opcodes for the
// opcode-based 1.26 client marshal).
//
// usage: activatewin [hold-s] [delay-s] [app-id]

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
static struct wl_seat       *g_seat;
static struct wl_keyboard   *g_kbd;
static struct wl_pointer    *g_ptr;
static uint32_t              g_serial; // last seat event serial (pointer enter/button)
static struct wl_seat       *g_serial_seat;
static int                   g_done;
static const char           *g_id   = "activatewin";
static int                   g_hold = 10;
static int                   g_delay = 0;
static char                  g_tokstr[128];

// ---- xdg-activation-v1, the fork's wire format ----------------------------
// manager: 0 destroy, 1 get_activation_token(n), 2 activate(s, o)
// token:   0 set_serial(u,o), 1 set_app_id(s), 2 set_surface(o),
//          3 commit, 4 destroy | events: 0 done(s)
//
// The compositor validates the token's set_serial against the client's own
// seat events (upstream 21290254's anti-spoofing): a token committed
// without a serial of this client's own pointer/keyboard events is
// rejected with an empty done. The gate moves a virtual pointer over the
// window before the ask so the fixture's enter event carries the serial.
// NO_SERIAL=1 skips the set_serial for the negative case: the ask must be
// rejected (no focus, no urgency).
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
static const struct wl_seat_listener g_seatl;

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
    else if (!strcmp(ifc, "wl_seat") && !g_seat) {
        g_seat = wl_registry_bind(r, id, &wl_seat_interface, 4);
        wl_seat_add_listener(g_seat, &g_seatl, NULL);
    }
}

// ---- the seat: a token needs this client's own event serial ---------------
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, int32_t x, int32_t y) {
    (void) d; (void) p; (void) s; (void) x; (void) y;
    g_serial      = serial;
    g_serial_seat = g_seat;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void) d; (void) p; (void) serial; (void) s; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, int32_t x, int32_t y) { (void) d; (void) p; (void) t; (void) x; (void) y; }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void) p; (void) t; (void) b; (void) st;
    if (serial)
        g_serial = serial;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void) d; (void) p; }
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, int32_t v) { (void) d; (void) p; (void) t; (void) a; (void) v; }
static const struct wl_pointer_listener g_ptrl = {
    .enter         = ptr_enter,
    .leave         = ptr_leave,
    .motion        = ptr_motion,
    .button        = ptr_button,
    .frame         = ptr_frame,
    .axis          = ptr_axis,
};

// keyboard events land in the same seat container as pointer events (the
// compositor records both per seat resource), so a key routed to this
// client is an equally valid serial source — and needs no pointer at all
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    (void) d; (void) k; (void) t; (void) key; (void) state;
    if (serial)
        g_serial = serial;
    g_serial_seat = g_seat;
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void) d; (void) k; (void) s; (void) keys;
    g_serial      = serial;
    g_serial_seat = g_seat;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void) d; (void) k; (void) serial; (void) s; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t t, uint32_t dep, uint32_t lat, uint32_t loc, uint32_t grp) { (void) d; (void) k; (void) t; (void) dep; (void) lat; (void) loc; (void) grp; }
// the keymap event is opcode 0 of wl_keyboard: a bound keyboard receives it
// on enter, and libwayland aborts on a NULL listener for it
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size) {
    (void) d; (void) k; (void) fmt; (void) size;
    close(fd);
}
// repeat_info is opcode 5 (since v4); libwayland aborts on a NULL listener
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t delay, int32_t rate) { (void) d; (void) k; (void) delay; (void) rate; }
static const struct wl_keyboard_listener g_kbdl = {
    .keymap     = kb_keymap,
    .key        = kb_key,
    .repeat_info = kb_repeat,
    .enter      = kb_enter,
    .leave      = kb_leave,
    .modifiers  = kb_mods,
};
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void) d; (void) s;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_ptr) {
        g_ptr = wl_seat_get_pointer(g_seat);
        if (g_ptr)
            wl_pointer_add_listener(g_ptr, &g_ptrl, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(g_seat);
        if (g_kbd)
            wl_keyboard_add_listener(g_kbd, &g_kbdl, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) { (void) d; (void) s; (void) name; }
static const struct wl_seat_listener g_seatl = { .capabilities = seat_caps, .name = seat_name };
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
        g_delay = atoi(argv[2]);
    if (argc > 3)
        g_id = argv[3];

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

    // optional delay between the map and the ask, dispatched wall-clock so
    // the map really lands (and the gate can move the compositor away in
    // the meantime) before the ask arrives at a not-visible window
    if (g_delay > 0) {
        struct timespec ts0 = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
        for (int i = 0; i < g_delay * 2 && !g_done; i++)
            if (wl_display_dispatch_timeout(g_dpy, &ts0) < 0)
                break;
    }

    // the ask — right after the map by default (the back-to-back burst a
    // tray-return performs). The "n" request is the constructor-style
    // marshal: the new interface is an argument, the id is minted by the
    // library (the 1.26 client API)
    struct wl_proxy *tok = wl_proxy_marshal_flags((struct wl_proxy *)g_act, 1, &xav_token_iface, 1, 0, NULL);
    if (!tok || wl_proxy_add_dispatcher(tok, tok_dispatch, NULL, NULL) != 0)
        return 1;

    // the compositor validates the token against this client's own seat
    // events; skip the set_serial under NO_SERIAL to exercise the rejection
    if (!g_serial || getenv("NO_SERIAL"))
        fprintf(stderr, "activatewin: no seat serial%s — token will be rejected\n",
                g_serial ? " (NO_SERIAL)" : "");
    else
        wl_proxy_marshal((struct wl_proxy *)tok, 0, g_serial, g_serial_seat); // set_serial

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
