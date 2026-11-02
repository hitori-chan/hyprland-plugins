#define _GNU_SOURCE
// A taskbar's pick over wlr-foreign-toplevel: activates the first toplevel
// with APP_ID on the first seat, as a taskbar or a window switcher does.
// usage: toplevel APP_ID
// exit 0 once the request is sent and a roundtrip has passed; 1 when no
// toplevel has that app id
#include <stdio.h>
#include <string.h>

#include "toplevel-proto.h"
#include <wayland-client.h>

static struct wl_seat                          *g_seat;
static struct zwlr_foreign_toplevel_manager_v1 *g_mgr;
static struct zwlr_foreign_toplevel_handle_v1  *g_match;
static const char                              *g_want;

static void h_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) {
    (void) d;
    (void) h;
    (void) t;
}
static void h_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id) {
    (void) d;
    if (!g_match && !strcmp(id, g_want))
        g_match = h;
}
static void h_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    (void) d;
    (void) h;
    (void) o;
}
static void h_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    (void) d;
    (void) h;
    (void) o;
}
static void h_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *s) {
    (void) d;
    (void) h;
    (void) s;
}
static void h_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void) d;
    (void) h;
}
static void h_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void) d;
    if (g_match == h)
        g_match = NULL;
}
static void h_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p) {
    (void) d;
    (void) h;
    (void) p;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener hl = {
    h_title, h_app_id, h_output_enter, h_output_leave, h_state, h_done, h_closed, h_parent,
};

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void) d;
    (void) m;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &hl, NULL);
}
static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {
    (void) d;
    (void) m;
}
static const struct zwlr_foreign_toplevel_manager_v1_listener ml = { m_toplevel, m_finished };

static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_seat") && !g_seat)
        g_seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
    else if (!strcmp(ifc, "zwlr_foreign_toplevel_manager_v1") && !g_mgr)
        g_mgr = wl_registry_bind(r, id, &zwlr_foreign_toplevel_manager_v1_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void) d;
    (void) r;
    (void) id;
}
static const struct wl_registry_listener regl = { reg_bind, reg_remove };

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: toplevel APP_ID\n");
        return 2;
    }
    g_want                 = argv[1];
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy)
        return 2;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_roundtrip(dpy);
    if (!g_seat || !g_mgr) {
        fprintf(stderr, "toplevel: no seat or foreign-toplevel manager\n");
        return 2;
    }
    zwlr_foreign_toplevel_manager_v1_add_listener(g_mgr, &ml, NULL);
    // the handles, then their app ids
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!g_match) {
        fprintf(stderr, "toplevel: no toplevel with app id %s\n", g_want);
        return 1;
    }
    zwlr_foreign_toplevel_handle_v1_activate(g_match, g_seat);
    wl_display_roundtrip(dpy);
    return 0;
}
