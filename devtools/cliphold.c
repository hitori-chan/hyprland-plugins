// cliphold — nested-gate clipboard source that deliberately delays transfer.
// It uses wlr-data-control so no surface or keyboard focus is needed.
//
//   cliphold DELAY_MS TEXT
//
// A negative delay holds the requested fd until the process or compositor
// exits. READY is printed after the selection belongs to this client; SEND is
// printed when a consumer requests it.
#define _GNU_SOURCE
#include <wayland-client.h>
#include "cliphold-proto.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct zwlr_data_control_manager_v1* manager;
static struct wl_seat*                      seat;
static int                                  send_fd = -1;
static int                                  delay_ms;
static int64_t                              send_at;
static const char*                          payload;
static int                                  cancelled;

static int64_t monotonicMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void registryAdd(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, zwlr_data_control_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, version < 2 ? version : 2);
    else if (!strcmp(interface, wl_seat_interface.name))
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registryListener = {registryAdd, registryRemove};

static void sourceSend(void* data, struct zwlr_data_control_source_v1* source, const char* mime, int32_t fd) {
    (void)data;
    (void)source;
    (void)mime;
    if (send_fd >= 0)
        close(send_fd);
    send_fd = fd;
    send_at = monotonicMs() + (delay_ms < 0 ? 0 : delay_ms);
    puts("SEND");
    fflush(stdout);
}

static void sourceCancelled(void* data, struct zwlr_data_control_source_v1* source) {
    (void)data;
    (void)source;
    cancelled = 1;
}

static const struct zwlr_data_control_source_v1_listener sourceListener = {sourceSend, sourceCancelled};

static void offerMime(void* data, struct zwlr_data_control_offer_v1* offer, const char* mime) {
    (void)data;
    (void)offer;
    (void)mime;
}

static const struct zwlr_data_control_offer_v1_listener offerListener = {offerMime};

static void deviceOffer(void* data, struct zwlr_data_control_device_v1* device, struct zwlr_data_control_offer_v1* offer) {
    (void)data;
    (void)device;
    zwlr_data_control_offer_v1_add_listener(offer, &offerListener, NULL);
}

static void deviceSelection(void* data, struct zwlr_data_control_device_v1* device, struct zwlr_data_control_offer_v1* offer) {
    (void)data;
    (void)device;
    if (offer)
        zwlr_data_control_offer_v1_destroy(offer);
}

static void deviceFinished(void* data, struct zwlr_data_control_device_v1* device) {
    (void)data;
    (void)device;
    cancelled = 1;
}

static void devicePrimary(void* data, struct zwlr_data_control_device_v1* device, struct zwlr_data_control_offer_v1* offer) {
    deviceSelection(data, device, offer);
}

static const struct zwlr_data_control_device_v1_listener deviceListener = {deviceOffer, deviceSelection, deviceFinished, devicePrimary};

static int writePayload(void) {
    const char* p    = payload;
    size_t      left = strlen(payload);
    while (left > 0) {
        const ssize_t n = write(send_fd, p, left);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(send_fd);
    send_fd = -1;
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cliphold DELAY_MS TEXT\n");
        return 2;
    }
    delay_ms = atoi(argv[1]);
    payload  = argv[2];
    signal(SIGPIPE, SIG_IGN);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 3;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager || !seat)
        return 4;

    struct zwlr_data_control_device_v1* device = zwlr_data_control_manager_v1_get_data_device(manager, seat);
    struct zwlr_data_control_source_v1* source = zwlr_data_control_manager_v1_create_data_source(manager);
    zwlr_data_control_device_v1_add_listener(device, &deviceListener, NULL);
    zwlr_data_control_source_v1_add_listener(source, &sourceListener, NULL);
    zwlr_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
    zwlr_data_control_device_v1_set_selection(device, source);
    if (wl_display_roundtrip(display) < 0)
        return 5;
    puts("READY");
    fflush(stdout);

    const int displayFd = wl_display_get_fd(display);
    int       wrote     = 0;
    while (!cancelled && !wrote) {
        if (send_fd >= 0 && delay_ms >= 0 && monotonicMs() >= send_at) {
            wrote = writePayload();
            break;
        }
        struct pollfd pfd = {.fd = displayFd, .events = POLLIN};
        int           timeout = -1;
        if (send_fd >= 0 && delay_ms >= 0) {
            const int64_t left = send_at - monotonicMs();
            timeout            = left <= 0 ? 0 : left > INT32_MAX ? INT32_MAX : (int)left;
        }
        const int ready = poll(&pfd, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && (pfd.revents & (POLLERR | POLLHUP))))
            break;
        if (ready > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(display) < 0)
            break;
        if (ready == 0)
            continue;
        wl_display_flush(display);
    }

    if (send_fd >= 0)
        close(send_fd);
    zwlr_data_control_source_v1_destroy(source);
    zwlr_data_control_device_v1_destroy(device);
    zwlr_data_control_manager_v1_destroy(manager);
    wl_seat_destroy(seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
