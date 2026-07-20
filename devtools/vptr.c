// vptr — a wlr-virtual-pointer injector. Reads a gesture script on stdin so a
// whole press/move/release lives in one process (the pointer dies with it).
//   argv: W H   (extent = monitor size in px; default 1280 800)
//   stdin lines: move X Y | press BTN | release BTN | scroll AXIS VAL | sleep MS
//   BTN = linux code (272 left, 273 right, 274 middle); AXIS 0=vert 1=horiz
#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>
#include "vptr-proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat                         *seat;

static void g_add(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, ver < 2 ? ver : 2);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void g_rem(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener RL = {g_add, g_rem};

static uint32_t ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
    uint32_t W = argc > 1 ? (uint32_t)atoi(argv[1]) : 1280;
    uint32_t H = argc > 2 ? (uint32_t)atoi(argv[2]) : 800;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "vptr: no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &RL, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr) { fprintf(stderr, "vptr: no zwlr_virtual_pointer_manager_v1\n"); return 2; }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(dpy);

    char line[512], cmd[32];
    long a, b;
    while (fgets(line, sizeof line, stdin)) {
        a = b = 0;
        if (sscanf(line, "%31s %ld %ld", cmd, &a, &b) < 1) continue;
        uint32_t t = ms();
        if (!strcmp(cmd, "move"))         { zwlr_virtual_pointer_v1_motion_absolute(p, t, (uint32_t)a, (uint32_t)b, W, H); zwlr_virtual_pointer_v1_frame(p); }
        else if (!strcmp(cmd, "press"))   { zwlr_virtual_pointer_v1_button(p, t, (uint32_t)a, 1); zwlr_virtual_pointer_v1_frame(p); }
        else if (!strcmp(cmd, "release")) { zwlr_virtual_pointer_v1_button(p, t, (uint32_t)a, 0); zwlr_virtual_pointer_v1_frame(p); }
        else if (!strcmp(cmd, "scroll"))  { zwlr_virtual_pointer_v1_axis(p, t, (uint32_t)a, wl_fixed_from_int((int)b)); zwlr_virtual_pointer_v1_frame(p); }
        else if (!strcmp(cmd, "sleep"))   { wl_display_flush(dpy); struct timespec ts = {a / 1000, (a % 1000) * 1000000L}; nanosleep(&ts, NULL); continue; }
        else continue;
        wl_display_flush(dpy);
    }
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
