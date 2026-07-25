// vkbd — a zwp_virtual_keyboard_v1 injector, vptr's twin for keys. Reads a
// script on stdin so a whole chord lives in one process (the keyboard dies
// with it), which is what keeps a stuck modifier from outliving the test.
//   stdin lines: tap KEY | press KEY | release KEY | sleep MS
//   KEY = a name below, or a raw linux evdev code
#define _GNU_SOURCE
#include <wayland-client.h>
#include "vkbd-proto.h"
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct zwp_virtual_keyboard_manager_v1* mgr;
static struct wl_seat*                         seat;

static void                                    g_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void                            g_rem(void* d, struct wl_registry* r, uint32_t name) {}
static const struct wl_registry_listener RL = {g_add, g_rem};

static uint32_t                          ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// evdev codes: the protocol carries the same values wl_keyboard.key does, so
// no +8 here — the compositor adds it before xkb sees it
static const struct {
    const char* n;
    uint32_t    c;
} KEYS[] = {
    {"esc", 1}, {"enter", 28}, {"space", 57}, {"up", 103}, {"down", 108}, {"delete", 111}, {"tab", 15}, {"a", 30},
};

static uint32_t keycode(const char* s) {
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++)
        if (!strcmp(s, KEYS[i].n))
            return KEYS[i].c;
    return (uint32_t)atoi(s);
}

// the protocol demands a keymap before any key event; the default xkb rules
// are the same ones the compositor would compile for a real keyboard
static int keymapFd(size_t* size) {
    struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap*  km  = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char*               s   = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    int                 fd  = -1;
    if (s) {
        *size = strlen(s) + 1;
        fd    = memfd_create("vkbd-keymap", MFD_CLOEXEC);
        if (fd >= 0 && ftruncate(fd, (off_t)*size) == 0) {
            void* p = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                memcpy(p, s, *size);
                munmap(p, *size);
            }
        }
        free(s);
    }
    if (km)
        xkb_keymap_unref(km);
    if (ctx)
        xkb_context_unref(ctx);
    return fd;
}

int main(void) {
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "vkbd: no display\n");
        return 1;
    }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &RL, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr) {
        fprintf(stderr, "vkbd: no zwp_virtual_keyboard_manager_v1\n");
        return 2;
    }
    struct zwp_virtual_keyboard_v1* kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(mgr, seat);

    size_t                          sz = 0;
    const int                       FD = keymapFd(&sz);
    if (FD < 0) {
        fprintf(stderr, "vkbd: no keymap\n");
        return 3;
    }
    zwp_virtual_keyboard_v1_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, FD, (uint32_t)sz);
    close(FD);
    zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0); // a defined state: no stuck modifier
    wl_display_roundtrip(dpy);

    char line[256], cmd[32], arg[64];
    while (fgets(line, sizeof line, stdin)) {
        arg[0] = 0;
        if (sscanf(line, "%31s %63s", cmd, arg) < 1)
            continue;
        if (!strcmp(cmd, "sleep")) {
            wl_display_flush(dpy);
            const long MS = atol(arg);
            struct timespec ts = {MS / 1000, (MS % 1000) * 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        const uint32_t CODE = keycode(arg);
        if (!strcmp(cmd, "press"))
            zwp_virtual_keyboard_v1_key(kb, ms(), CODE, WL_KEYBOARD_KEY_STATE_PRESSED);
        else if (!strcmp(cmd, "release"))
            zwp_virtual_keyboard_v1_key(kb, ms(), CODE, WL_KEYBOARD_KEY_STATE_RELEASED);
        else if (!strcmp(cmd, "tap")) {
            zwp_virtual_keyboard_v1_key(kb, ms(), CODE, WL_KEYBOARD_KEY_STATE_PRESSED);
            wl_display_flush(dpy);
            struct timespec ts = {0, 30 * 1000000L};
            nanosleep(&ts, NULL);
            zwp_virtual_keyboard_v1_key(kb, ms(), CODE, WL_KEYBOARD_KEY_STATE_RELEASED);
        } else
            continue;
        wl_display_flush(dpy);
    }

    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
