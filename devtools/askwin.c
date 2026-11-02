#define _GNU_SOURCE
// An app asking to be activated over xdg-activation, as a browser's
// "switch to tab" or a chat app's notification click does: maps a window,
// and on each `ask` line on stdin requests a token (with the latest seat
// serial it was sent) and activates its own surface. Prints `asked` once
// the compositor has handled the request, `rejected` for an empty token.
// Exits at the end of stdin.
// usage: askwin APP_ID
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "activation-proto.h"
#include "fixwin-protocol.h"
#include <wayland-client.h>

#define W 320
#define H 240

static struct wl_compositor   *g_comp;
static struct wl_shm          *g_shm;
static struct xdg_wm_base     *g_wm;
static struct wl_seat         *g_seat;
static struct xdg_activation_v1 *g_act;
static struct wl_surface      *g_surf;
static uint32_t                g_serial, g_cfgSerial;
static int                     g_configured;

static void reg_bind(void *d, struct wl_registry *r, uint32_t id, const char *ifc, uint32_t ver) {
    (void) d;
    (void) ver;
    if (!strcmp(ifc, "wl_compositor") && !g_comp)
        g_comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(ifc, "wl_shm") && !g_shm)
        g_shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(ifc, "xdg_wm_base") && !g_wm)
        g_wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(ifc, "wl_seat") && !g_seat)
        g_seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
    else if (!strcmp(ifc, "xdg_activation_v1") && !g_act)
        g_act = wl_registry_bind(r, id, &xdg_activation_v1_interface, 1);
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
    g_cfgSerial  = serial;
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

// every serial the seat sends this client is one a token may carry
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
    (void) d;
    (void) k;
    (void) f;
    (void) s;
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void) d;
    (void) k;
    (void) s;
    (void) keys;
    g_serial = serial;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void) d;
    (void) k;
    (void) s;
    g_serial = serial;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) {
    (void) d;
    (void) k;
    (void) t;
    (void) key;
    (void) st;
    g_serial = serial;
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void) d;
    (void) k;
    (void) a;
    (void) b;
    (void) c;
    (void) g;
    g_serial = serial;
}
static const struct wl_keyboard_listener kbl = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, NULL };

static char g_token[256];
static int  g_tokenDone;
static void tok_done(void *d, struct xdg_activation_token_v1 *t, const char *token) {
    (void) d;
    (void) t;
    snprintf(g_token, sizeof g_token, "%s", token);
    g_tokenDone = 1;
}
static const struct xdg_activation_token_v1_listener tokl = { tok_done };

static struct wl_buffer *solid(void) {
    int fd = memfd_create("askwin", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, W * H * 4) < 0)
        return NULL;
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, W * H * 4);
    struct wl_buffer   *b    = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void ask(struct wl_display *dpy) {
    struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(g_act);
    xdg_activation_token_v1_add_listener(t, &tokl, NULL);
    xdg_activation_token_v1_set_serial(t, g_serial, g_seat);
    xdg_activation_token_v1_set_surface(t, g_surf);
    g_tokenDone = 0;
    xdg_activation_token_v1_commit(t);
    while (!g_tokenDone)
        if (wl_display_dispatch(dpy) < 0)
            return;
    xdg_activation_token_v1_destroy(t);
    if (!g_token[0]) {
        printf("rejected\n");
        fflush(stdout);
        return;
    }
    xdg_activation_v1_activate(g_act, g_token, g_surf);
    wl_display_roundtrip(dpy);
    printf("asked\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: askwin APP_ID\n");
        return 2;
    }
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy)
        return 2;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &regl, NULL);
    wl_display_roundtrip(dpy);
    if (!g_comp || !g_shm || !g_wm || !g_seat || !g_act) {
        fprintf(stderr, "askwin: missing a global (xdg-activation or a seat)\n");
        return 2;
    }
    xdg_wm_base_add_listener(g_wm, &wml, NULL);
    wl_keyboard_add_listener(wl_seat_get_keyboard(g_seat), &kbl, NULL);

    g_surf                    = wl_compositor_create_surface(g_comp);
    struct xdg_surface  *xs  = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_surface_add_listener(xs, &xsl, NULL);
    xdg_toplevel_add_listener(top, &topl, NULL);
    xdg_toplevel_set_app_id(top, argv[1]);
    wl_surface_commit(g_surf);
    while (!g_configured)
        if (wl_display_dispatch(dpy) < 0)
            return 2;
    xdg_surface_ack_configure(xs, g_cfgSerial);
    wl_surface_attach(g_surf, solid(), 0, 0);
    wl_surface_commit(g_surf);
    wl_display_flush(dpy);

    char line[64];
    for (;;) {
        struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        wl_display_flush(dpy);
        if (poll(p, 2, -1) < 0)
            return 1;
        if (p[0].revents & POLLIN && wl_display_dispatch(dpy) < 0)
            return 1;
        if (p[1].revents & (POLLIN | POLLHUP)) {
            if (!fgets(line, sizeof line, stdin))
                return 0;
            if (!strncmp(line, "ask", 3))
                ask(dpy);
        }
        if (g_configured) {
            g_configured = 0;
            xdg_surface_ack_configure(xs, g_cfgSerial);
            wl_surface_commit(g_surf);
        }
    }
}
