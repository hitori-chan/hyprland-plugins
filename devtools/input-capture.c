// input-capture - a small hyprland-input-capture-v1 receiver for the nested gate.
// It owns no compositor state: it only proves that an active native capture
// session receives input after plugin listeners have had a chance to observe it.
#define _POSIX_C_SOURCE 200809L

#include "input-capture-proto.h"
#include <libei.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

struct capture_state {
	struct wl_display *display;
	struct hyprland_input_capture_manager_v1 *manager;
	struct hyprland_input_capture_v1 *session;
	struct ei *ei;
	int eis_fd;
	bool eis_connected;
	bool pointer_device;
	bool keyboard_device;
	bool activated;
	uint32_t activation_id;
	unsigned motion_events;
	unsigned button_presses;
	unsigned button_releases;
	unsigned key_presses;
	unsigned key_releases;
};

static uint64_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void registry_add(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	struct capture_state *state = data;
	if (strcmp(interface, hyprland_input_capture_manager_v1_interface.name) != 0)
		return;

	state->manager = wl_registry_bind(registry, name, &hyprland_input_capture_manager_v1_interface, version < 1 ? version : 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_add,
	.global_remove = registry_remove,
};

static void session_eis_fd(void *data, struct hyprland_input_capture_v1 *session, int32_t fd) {
	struct capture_state *state = data;
	(void)session;
	state->eis_fd = fd;
}

static void session_disabled(void *data, struct hyprland_input_capture_v1 *session) {
	struct capture_state *state = data;
	(void)session;
	state->activated = false;
}

static void session_activated(void *data, struct hyprland_input_capture_v1 *session, uint32_t activation_id, wl_fixed_t x, wl_fixed_t y, uint32_t barrier_id) {
	struct capture_state *state = data;
	(void)session;
	(void)x;
	(void)y;
	(void)barrier_id;
	state->activated = true;
	state->activation_id = activation_id;
}

static void session_deactivated(void *data, struct hyprland_input_capture_v1 *session, uint32_t activation_id) {
	struct capture_state *state = data;
	(void)session;
	(void)activation_id;
	state->activated = false;
}

static const struct hyprland_input_capture_v1_listener session_listener = {
	.eis_fd = session_eis_fd,
	.disabled = session_disabled,
	.activated = session_activated,
	.deactivated = session_deactivated,
};

static void process_ei_event(struct capture_state *state, struct ei_event *event) {
	switch (ei_event_get_type(event)) {
	case EI_EVENT_CONNECT:
		state->eis_connected = true;
		break;
	case EI_EVENT_SEAT_ADDED: {
		struct ei_seat *seat = ei_event_get_seat(event);
		if (seat)
			ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, NULL);
		break;
	}
	case EI_EVENT_DEVICE_ADDED: {
		struct ei_device *device = ei_event_get_device(event);
		if (!device)
			break;
		if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) && ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON) &&
			ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL))
			state->pointer_device = true;
		if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD))
			state->keyboard_device = true;
		break;
	}
	case EI_EVENT_DEVICE_REMOVED:
		state->pointer_device = false;
		state->keyboard_device = false;
		break;
	case EI_EVENT_POINTER_MOTION:
		state->motion_events++;
		break;
	case EI_EVENT_BUTTON_BUTTON:
		if (ei_event_button_get_button(event) != 272)
			break;
		if (ei_event_button_get_is_press(event))
			state->button_presses++;
		else
			state->button_releases++;
		break;
	case EI_EVENT_KEYBOARD_KEY:
		if (ei_event_keyboard_get_key_is_press(event))
			state->key_presses++;
		else
			state->key_releases++;
		break;
	default:
		break;
	}
}

static bool dispatch_ei(struct capture_state *state) {
	ei_dispatch(state->ei);
	for (;;) {
		struct ei_event *event = ei_get_event(state->ei);
		if (!event)
			return true;
		if (ei_event_get_type(event) == EI_EVENT_DISCONNECT) {
			ei_event_unref(event);
			return false;
		}
		process_ei_event(state, event);
		ei_event_unref(event);
	}
}

static bool wait_for_ei_devices(struct capture_state *state, uint64_t deadline) {
	while (monotonic_ms() < deadline) {
		if (!dispatch_ei(state))
			return false;
		if (state->eis_connected && state->pointer_device && state->keyboard_device)
			return true;

		const uint64_t now = monotonic_ms();
		const int timeout = now < deadline && deadline - now < INT32_MAX ? (int)(deadline - now) : INT32_MAX;
		struct pollfd pfd = {.fd = ei_get_fd(state->ei), .events = POLLIN};
		if (poll(&pfd, 1, timeout) < 0 && errno != EINTR)
			return false;
	}
	return false;
}

static bool wait_for_capture(struct capture_state *state, uint64_t deadline) {
	const int wayland_fd = wl_display_get_fd(state->display);
	const int ei_fd = ei_get_fd(state->ei);
	while (monotonic_ms() < deadline) {
		wl_display_dispatch_pending(state->display);
		if (wl_display_get_error(state->display) != 0)
			return false;
		if (!dispatch_ei(state))
			return false;
		if (state->activated && state->motion_events > 0 && state->button_presses > 0 && state->button_releases > 0 && state->key_presses > 0 &&
			state->key_releases > 0)
			return true;

		const uint64_t now = monotonic_ms();
		const int timeout = now < deadline && deadline - now < INT32_MAX ? (int)(deadline - now) : INT32_MAX;
		struct pollfd fds[2] = {
			{.fd = wayland_fd, .events = POLLIN},
			{.fd = ei_fd, .events = POLLIN},
		};
		if (poll(fds, 2, timeout) < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			return false;
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(state->display) < 0)
				return false;
		}
		if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
			return false;
		if (fds[1].revents & POLLIN) {
			if (!dispatch_ei(state))
				return false;
		}
	}
	return false;
}

static void usage(const char *name) {
	fprintf(stderr, "usage: %s [monitor-width monitor-height]\n", name);
}

int main(int argc, char **argv) {
	uint32_t width = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1280;
	uint32_t height = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 800;
	if (argc > 3 || width < 2 || height < 2) {
		usage(argv[0]);
		return 2;
	}

	struct capture_state state = {
		.display = wl_display_connect(NULL),
		.eis_fd = -1,
	};
	if (!state.display) {
		fprintf(stderr, "input-capture: no Wayland display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) < 0 || !state.manager) {
		fprintf(stderr, "input-capture: protocol is unavailable\n");
		wl_display_disconnect(state.display);
		return 1;
	}

	state.session = hyprland_input_capture_manager_v1_create_session(state.manager, "hyprland-plugins-stress");
	if (!state.session || hyprland_input_capture_v1_add_listener(state.session, &session_listener, &state) < 0 || wl_display_roundtrip(state.display) < 0 || state.eis_fd < 0) {
		fprintf(stderr, "input-capture: session setup failed\n");
		wl_display_disconnect(state.display);
		return 1;
	}

	state.ei = ei_new_receiver(NULL);
	if (!state.ei) {
		fprintf(stderr, "input-capture: cannot create libei receiver\n");
		wl_display_disconnect(state.display);
		return 1;
	}
	ei_configure_name(state.ei, "hyprland-plugins-stress");
	if (ei_setup_backend_fd(state.ei, state.eis_fd) < 0 || !wait_for_ei_devices(&state, monotonic_ms() + 3000)) {
		fprintf(stderr, "input-capture: EIS setup failed\n");
		ei_unref(state.ei);
		wl_display_disconnect(state.display);
		return 1;
	}

	hyprland_input_capture_v1_add_barrier(state.session, 1, 1, 0, 0, width - 1, 0);
	hyprland_input_capture_v1_enable(state.session);
	if (wl_display_flush(state.display) < 0 || wl_display_roundtrip(state.display) < 0) {
		fprintf(stderr, "input-capture: enable failed\n");
		ei_unref(state.ei);
		wl_display_disconnect(state.display);
		return 1;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	puts("READY");
	const bool complete = wait_for_capture(&state, monotonic_ms() + 8000);
	if (state.activated) {
		hyprland_input_capture_v1_release(state.session, state.activation_id, wl_fixed_from_int(-1), wl_fixed_from_int(-1));
		wl_display_flush(state.display);
	}

	const bool result = complete && state.motion_events > 0 && state.button_presses > 0 && state.button_releases > 0 && state.key_presses > 0 &&
		state.key_releases > 0;
	if (!result)
		fprintf(stderr, "input-capture: timed out (active=%d motion=%u button=%u/%u key=%u/%u)\n", state.activated, state.motion_events, state.button_presses,
			state.button_releases, state.key_presses, state.key_releases);

	ei_unref(state.ei);
	wl_display_disconnect(state.display);
	return result ? 0 : 1;
}
