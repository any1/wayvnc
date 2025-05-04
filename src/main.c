/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <neatvnc.h>
#include <aml.h>
#include <signal.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#include <pixman.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "wlr-screencopy-unstable-v1.h"
#include "ext-image-copy-capture-v1.h"
#include "ext-image-capture-source-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "virtual-keyboard-unstable-v1.h"
#include "xdg-output-unstable-v1.h"
#include "wlr-output-power-management-unstable-v1.h"
#include "wlr-output-management-unstable-v1.h"
#include "linux-dmabuf-unstable-v1.h"
#include "ext-transient-seat-v1.h"
#include "screencopy-interface.h"
#include "data-control.h"
#include "strlcpy.h"
#include "output.h"
#include "output-management.h"
#include "pointer.h"
#include "keyboard.h"
#include "seat.h"
#include "cfg.h"
#include "transform-util.h"
#include "usdt.h"
#include "ctl-server.h"
#include "util.h"
#include "option-parser.h"
#include "pixels.h"
#include "buffer.h"
#include "time-util.h"

#ifdef ENABLE_PAM
#include "pam_auth.h"
#endif

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

#define XSTR(x) STR(x)
#define STR(x) #x

#define MAYBE_UNUSED __attribute__((unused))

struct wayvnc_client;

enum socket_type {
	SOCKET_TYPE_TCP = 0,
	SOCKET_TYPE_UNIX,
	SOCKET_TYPE_WEBSOCKET,
	SOCKET_TYPE_FROM_FD,
};

struct wayvnc {
	bool do_exit;

	struct wl_display* display;
	struct wl_registry* registry;
	struct aml_handler* wl_handler;
	struct wl_list outputs;
	struct wl_list seats;
	struct cfg cfg;

	struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
	struct zwlr_virtual_pointer_manager_v1* pointer_manager;
	struct zwlr_data_control_manager_v1* data_control_manager;
	struct ext_transient_seat_manager_v1* transient_seat_manager;

	struct output* selected_output;
	struct seat* selected_seat;

	struct screencopy* screencopy;

	struct aml_handler* wayland_handler;
	struct aml_signal* signal_handler;

	struct nvnc* nvnc;
	struct nvnc_display* nvnc_display;

	const char* kb_layout;
	const char* kb_variant;

	uint32_t damage_area_sum;
	uint32_t n_frames_captured;
	uint32_t n_frames_sent;

	bool disable_input;
	bool use_transient_seat;

	int nr_clients;
	struct aml_ticker* performance_ticker;

	struct aml_timer* capture_retry_timer;

	struct ctl* ctl;
	bool is_initializing;

	bool start_detached;
	bool overlay_cursor;
	int max_rate;
	bool enable_gpu_features;
	bool enable_resizing;

	struct wayvnc_client* master_layout_client;

	struct wayvnc_client* cursor_master;
	struct screencopy* cursor_sc;

	uint64_t last_send_time;
	struct aml_timer* rate_limiter;
	struct wv_buffer* next_frame;
};

struct wayvnc_client {
	struct wayvnc* server;
	struct nvnc_client* nvnc_client;

	struct seat* seat;
	struct ext_transient_seat_v1* transient_seat;

	unsigned id;
	struct pointer pointer;
	struct keyboard keyboard;
	struct data_control data_control;
};

void wayvnc_exit(struct wayvnc* self);
void on_capture_done(enum screencopy_result result, struct wv_buffer* buffer,
		void* userdata);
static void on_cursor_capture_done(enum screencopy_result result,
		struct wv_buffer* buffer, void* userdata);
static void on_nvnc_client_new(struct nvnc_client* client);
void switch_to_output(struct wayvnc*, struct output*);
void switch_to_next_output(struct wayvnc*);
void switch_to_prev_output(struct wayvnc*);
static void client_init_seat(struct wayvnc_client* self);
static void client_init_pointer(struct wayvnc_client* self);
static void client_init_keyboard(struct wayvnc_client* self);
static void client_init_data_control(struct wayvnc_client* self);
static void client_detach_wayland(struct wayvnc_client* self);
static int blank_screen(struct wayvnc* self);
static bool wayland_attach(struct wayvnc* self, const char* display,
		const char* output);
static void wayland_detach(struct wayvnc* self);
static bool configure_cursor_sc(struct wayvnc* self,
		struct wayvnc_client* client);

struct wl_shm* wl_shm = NULL;
struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf = NULL;
struct zxdg_output_manager_v1* xdg_output_manager = NULL;
struct zwlr_output_power_manager_v1* wlr_output_power_manager = NULL;
struct zwlr_screencopy_manager_v1* screencopy_manager = NULL;
struct ext_output_image_capture_source_manager_v1*
		ext_output_image_capture_source_manager = NULL;
struct ext_image_copy_capture_manager_v1* ext_image_copy_capture_manager = NULL;

extern struct screencopy_impl wlr_screencopy_impl, ext_image_copy_capture_impl;

static bool registry_add_input(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface,
			 uint32_t version)
{
	struct wayvnc* self = data;

	if (self->disable_input)
		return false;

	if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat* wl_seat =
			wl_registry_bind(registry, id, &wl_seat_interface, 7);
		if (!wl_seat)
			return true;

		struct seat* seat = seat_new(wl_seat, id);
		if (!seat) {
			wl_seat_release(wl_seat);
			return true;
		}

		wl_list_insert(&self->seats, &seat->link);
		return true;
	}

	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		self->pointer_manager = wl_registry_bind(registry, id,
				&zwlr_virtual_pointer_manager_v1_interface,
				MIN(2, version));
		return true;
	}

	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		self->keyboard_manager = wl_registry_bind(registry, id,
				&zwp_virtual_keyboard_manager_v1_interface,
				1);
		return true;
	}

	if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
		self->data_control_manager = wl_registry_bind(registry, id,
				&zwlr_data_control_manager_v1_interface, 2);
		return true;
	}

	if (strcmp(interface, ext_transient_seat_manager_v1_interface.name) == 0) {
		self->transient_seat_manager = wl_registry_bind(registry, id,
				&ext_transient_seat_manager_v1_interface, 1);
		return true;
	}

	return false;
}

static void registry_add(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface,
			 uint32_t version)
{
	struct wayvnc* self = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		nvnc_trace("Registering new output %u", id);
		struct wl_output* wl_output =
			wl_registry_bind(registry, id, &wl_output_interface, 3);
		if (!wl_output)
			return;

		struct output* output = output_new(wl_output, id);
		if (!output)
			return;

		wl_list_insert(&self->outputs, &output->link);
		if (!self->is_initializing) {
			wl_display_dispatch(self->display);
			wl_display_roundtrip(self->display);

			ctl_server_event_output_added(self->ctl, output->name);
		}

		return;
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new xdg_output_manager");
		xdg_output_manager =
			wl_registry_bind(registry, id,
			                 &zxdg_output_manager_v1_interface, 3);

		output_setup_xdg_output_managers(&self->outputs);
		return;
	}

	if (strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new wlr_output_power_manager");
		wlr_output_power_manager = wl_registry_bind(registry, id,
					&zwlr_output_power_manager_v1_interface, 1);
		return;
	}

	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new wlr_output_manager");
		struct zwlr_output_manager_v1* wlr_output_manager =
			wl_registry_bind(registry, id,
				&zwlr_output_manager_v1_interface, 1);
		wlr_output_manager_setup(wlr_output_manager);
		return;
	}

	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		screencopy_manager =
			wl_registry_bind(registry, id,
					 &zwlr_screencopy_manager_v1_interface,
					 MIN(3, version));
		return;
	}

#if 1
	if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		ext_image_copy_capture_manager =
			wl_registry_bind(registry, id,
					 &ext_image_copy_capture_manager_v1_interface,
					 MIN(1, version));
		return;
	}

	if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		ext_output_image_capture_source_manager =
			wl_registry_bind(registry, id,
					&ext_output_image_capture_source_manager_v1_interface,
					MIN(1, version));
		return;
	}
#endif

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
		return;
	}

	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		zwp_linux_dmabuf = wl_registry_bind(registry, id,
				&zwp_linux_dmabuf_v1_interface, 3);
		return;
	}

	if (registry_add_input(data, registry, id, interface, version))
		return;
}

static void disconnect_seat_clients(struct wayvnc* self, struct seat* seat)
{
	struct nvnc_client* nvnc_client;
	for (nvnc_client = nvnc_client_first(self->nvnc); nvnc_client;
			nvnc_client = nvnc_client_next(nvnc_client)) {
		struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);
		assert(client);

		if (client->seat == seat) {
			nvnc_client_close(nvnc_client);
		}
	}
}

static void registry_remove(void* data, struct wl_registry* registry,
			    uint32_t id)
{
	struct wayvnc* self = data;

	struct output* out = output_find_by_id(&self->outputs, id);
	if (out) {
		if (out == self->selected_output) {
			nvnc_log(NVNC_LOG_WARNING, "Selected output %s went away",
					out->name);
			switch_to_prev_output(self);
		} else
			nvnc_log(NVNC_LOG_INFO, "Output %s went away", out->name);

		ctl_server_event_output_removed(self->ctl, out->name);

		wl_list_remove(&out->link);
		output_destroy(out);

		if (out == self->selected_output) {
			if (self->start_detached) {
				nvnc_log(NVNC_LOG_WARNING, "No fallback outputs left. Detaching...");
				wayland_detach(self);
			} else {
				nvnc_log(NVNC_LOG_ERROR, "No fallback outputs left. Exiting...");
				wayvnc_exit(self);
			}
		}

		return;
	}

	struct seat* seat = seat_find_by_id(&self->seats, id);
	if (seat) {
		nvnc_log(NVNC_LOG_INFO, "Seat %s went away", seat->name);
		disconnect_seat_clients(self, seat);
		wl_list_remove(&seat->link);
		seat_destroy(seat);
		return;
	}
}

static void wayland_detach(struct wayvnc* self)
{
	if (!self->display)
		return;

	aml_stop(aml_get_default(), self->wl_handler);
	aml_unref(self->wl_handler);
	self->wl_handler = NULL;

	// Screen blanking is required to release wl_shm of linux_dmabuf.
	if (self->nvnc)
		blank_screen(self);

	if (self->nvnc) {
		struct nvnc_client* nvnc_client;
		for (nvnc_client = nvnc_client_first(self->nvnc); nvnc_client;
				nvnc_client = nvnc_client_next(nvnc_client)) {
			struct wayvnc_client* client =
				nvnc_get_userdata(nvnc_client);
			client_detach_wayland(client);
		}
	}

	self->selected_output = NULL;

	output_list_destroy(&self->outputs);
	seat_list_destroy(&self->seats);

	if (zwp_linux_dmabuf)
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf);
	zwp_linux_dmabuf = NULL;

	screencopy_stop(self->screencopy);
	screencopy_destroy(self->screencopy);
	self->screencopy = NULL;

	screencopy_stop(self->cursor_sc);
	screencopy_destroy(self->cursor_sc);
	self->cursor_sc = NULL;

	if (xdg_output_manager)
		zxdg_output_manager_v1_destroy(xdg_output_manager);
	xdg_output_manager = NULL;

	if (wlr_output_power_manager)
		zwlr_output_power_manager_v1_destroy(wlr_output_power_manager);
	wlr_output_power_manager = NULL;

	wlr_output_manager_destroy();

	wl_shm_destroy(wl_shm);
	wl_shm = NULL;

	if (self->keyboard_manager)
		zwp_virtual_keyboard_manager_v1_destroy(self->keyboard_manager);
	self->keyboard_manager = NULL;

	if (self->pointer_manager)
		zwlr_virtual_pointer_manager_v1_destroy(self->pointer_manager);
	self->pointer_manager = NULL;

	if (self->data_control_manager)
		zwlr_data_control_manager_v1_destroy(self->data_control_manager);
	self->data_control_manager = NULL;

	if (self->performance_ticker) {
		aml_stop(aml_get_default(), self->performance_ticker);
		aml_unref(self->performance_ticker);
	}
	self->performance_ticker = NULL;

	if (screencopy_manager)
		zwlr_screencopy_manager_v1_destroy(screencopy_manager);
	screencopy_manager = NULL;

	if (ext_output_image_capture_source_manager)
		ext_output_image_capture_source_manager_v1_destroy(
				ext_output_image_capture_source_manager);
	ext_output_image_capture_source_manager = NULL;

	if (ext_image_copy_capture_manager)
		ext_image_copy_capture_manager_v1_destroy(ext_image_copy_capture_manager);
	ext_image_copy_capture_manager = NULL;

	if (self->capture_retry_timer) {
		aml_stop(aml_get_default(), self->capture_retry_timer);
		aml_unref(self->capture_retry_timer);
	}
	self->capture_retry_timer = NULL;

	if (self->transient_seat_manager)
		ext_transient_seat_manager_v1_destroy(self->transient_seat_manager);

	wl_registry_destroy(self->registry);
	self->registry = NULL;

	wl_display_disconnect(self->display);
	self->display = NULL;

	if (self->ctl)
		ctl_server_event_detached(self->ctl);
}

void on_wayland_event(struct aml_handler* handler)
{
	struct wayvnc* self = aml_get_userdata(handler);

	int rc MAYBE_UNUSED = wl_display_prepare_read(self->display);
	assert(rc == 0);

	if (wl_display_read_events(self->display) < 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			nvnc_log(NVNC_LOG_ERROR, "Compositor has gone away. Exiting...");
			if (self->start_detached)
				wayland_detach(self);
			else
				wayvnc_exit(self);
			return;
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to read wayland events: %m");
		}
	}

	if (wl_display_dispatch_pending(self->display) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to dispatch pending");
		wayland_detach(self);
		// TODO: Re-attach
	}
}

static int init_wayland(struct wayvnc* self, const char* display)
{
	self->is_initializing = true;
	static const struct wl_registry_listener registry_listener = {
		.global = registry_add,
		.global_remove = registry_remove,
	};

	self->display = wl_display_connect(display);
	if (!self->display) {
		const char* display_name = display ? display:
			getenv("WAYLAND_DISPLAY");
		if (!display_name) {
			nvnc_log(NVNC_LOG_ERROR, "WAYLAND_DISPLAY is not set in the environment");
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to connect to WAYLAND_DISPLAY=\"%s\"", display_name);
			nvnc_log(NVNC_LOG_ERROR, "Ensure wayland is running with that display name");
		}
		return -1;
	}

	wl_list_init(&self->outputs);
	wl_list_init(&self->seats);

	self->registry = wl_display_get_registry(self->display);
	if (!self->registry) {
		nvnc_log(NVNC_LOG_ERROR, "Could not locate the wayland compositor object registry");
		goto failure;
	}

	wl_registry_add_listener(self->registry, &registry_listener, self);

	wl_display_dispatch(self->display);
	wl_display_roundtrip(self->display);
	self->is_initializing = false;

	if (!self->pointer_manager && !self->disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "Virtual Pointer protocol not supported by compositor.");
		nvnc_log(NVNC_LOG_ERROR, "wayvnc may still work if started with --disable-input.");
		goto failure;
	}

	if (!self->keyboard_manager && !self->disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "Virtual Keyboard protocol not supported by compositor.");
		nvnc_log(NVNC_LOG_ERROR, "wayvnc may still work if started with --disable-input.");
		goto failure;
	}

	if (!screencopy_manager && !ext_image_copy_capture_manager) {
		nvnc_log(NVNC_LOG_ERROR, "Screencopy protocol not supported by compositor. Exiting. Refer to FAQ section in man page.");
		goto failure;
	}

	if (!self->transient_seat_manager && self->use_transient_seat) {
		nvnc_log(NVNC_LOG_ERROR, "Transient seat protocol not supported by compositor");
		goto failure;
	}

	self->wl_handler = aml_handler_new(wl_display_get_fd(self->display),
	                             on_wayland_event, self, NULL);
	if (!self->wl_handler)
		goto failure;

	int rc = aml_start(aml_get_default(), self->wl_handler);
	if (rc < 0)
		goto handler_failure;

	return 0;

failure:
	wl_display_disconnect(self->display);
	self->display = NULL;
handler_failure:
	if (self->wl_handler)
		aml_unref(self->wl_handler);
	self->wl_handler = NULL;
	return -1;
}

void wayvnc_exit(struct wayvnc* self)
{
	self->do_exit = true;
}

void on_signal(struct aml_signal* obj)
{
	nvnc_log(NVNC_LOG_INFO, "Received termination signal.");
	struct wayvnc* self = aml_get_userdata(obj);
	wayvnc_exit(self);
}

struct cmd_response* on_output_cycle(struct ctl* ctl, enum output_cycle_direction direction)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	nvnc_log(NVNC_LOG_INFO, "ctl command: Rotating to %s output",
			direction == OUTPUT_CYCLE_FORWARD ? "next" : "previous");
	if (!self->display)
		return cmd_failed("Not attached!");
	struct output* next = output_cycle(&self->outputs,
			self->selected_output, direction);
	switch_to_output(self, next);
	return cmd_ok();
}

struct cmd_response* on_output_switch(struct ctl* ctl,
			const char* output_name)
{
	nvnc_log(NVNC_LOG_INFO, "ctl command: Switch to output \"%s\"", output_name);

	struct wayvnc* self = ctl_server_userdata(ctl);
	if (!self->display)
		return cmd_failed("Not attached!");
	if (!output_name || output_name[0] == '\0')
		return cmd_failed("Output name is required");
	struct output* output = output_find_by_name(&self->outputs, output_name);
	if (!output) {
		return cmd_failed("No such output \"%s\"", output_name);
	}
	switch_to_output(self, output);
	return cmd_ok();
}

static struct ctl_server_client *client_next(struct ctl* ctl,
		struct ctl_server_client *prev)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	struct nvnc_client* vnc_prev = (struct nvnc_client*)prev;

	return prev ? (struct ctl_server_client*)nvnc_client_next(vnc_prev) :
		(struct ctl_server_client*)nvnc_client_first(self->nvnc);
}

static void compose_client_info(const struct wayvnc_client* client,
		struct ctl_server_client_info* info)
{
	info->id = client->id;
	socklen_t addrlen = sizeof(info->address_storage);
	nvnc_client_get_address(client->nvnc_client,
			(struct sockaddr*)&info->address_storage, &addrlen);
	info->username = nvnc_client_get_auth_username(client->nvnc_client);
	info->seat = client->seat ? client->seat->name : NULL;
}

static void client_info(const struct ctl_server_client* client_handle,
		struct ctl_server_client_info* info)
{
	const struct nvnc_client *vnc_client =
		(const struct nvnc_client*)client_handle;
	const struct wayvnc_client *client = nvnc_get_userdata(vnc_client);
	compose_client_info(client, info);
}

static int get_output_list(struct ctl* ctl,
		struct ctl_server_output** outputs)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	if (!self->display) {
		*outputs = NULL;
		return 0;
	}
	int n = wl_list_length(&self->outputs);
	if (n == 0) {
		*outputs = NULL;
		return 0;
	}
	*outputs = calloc(n, sizeof(**outputs));
	struct output* output;
	struct ctl_server_output* item = *outputs;
	wl_list_for_each(output, &self->outputs, link) {
		strlcpy(item->name, output->name, sizeof(item->name));
		strlcpy(item->description, output->description,
				sizeof(item->description));
		item->height = output->height;
		item->width = output->width;
		item->captured = (output->id == self->selected_output->id);
		strlcpy(item->power, output_power_state_name(output->power),
				sizeof(item->power));
		item++;
	}
	return n;
}

static struct cmd_response* on_disconnect_client(struct ctl* ctl,
		const char* id_string)
{
	char* endptr;
	unsigned int id = strtoul(id_string, &endptr, 0);
	if (!*id_string || *endptr)
		return cmd_failed("Invalid client ID \"%s\"", id_string);

	struct wayvnc* self = ctl_server_userdata(ctl);
	for (struct nvnc_client* nvnc_client = nvnc_client_first(self->nvnc);
			nvnc_client;
			nvnc_client = nvnc_client_next(nvnc_client)) {
		struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);
		if (client->id == id) {
			nvnc_log(NVNC_LOG_WARNING, "Disconnecting client %d via control socket command",
					client->id);
			nvnc_client_close(nvnc_client);
			return cmd_ok();
		}
	}
	return cmd_failed("No such client with ID \"%s\"", id_string);
}

static struct cmd_response* on_wayvnc_exit(struct ctl* ctl)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	nvnc_log(NVNC_LOG_WARNING, "Shutting down via control socket command");
	wayvnc_exit(self);
	return cmd_ok();
}

int init_main_loop(struct wayvnc* self)
{
	struct aml* loop = aml_get_default();

	struct aml_signal* sig;
	sig = aml_signal_new(SIGINT, on_signal, self, NULL);
	if (!sig)
		return -1;

	int rc = aml_start(loop, sig);
	aml_unref(sig);
	if (rc < 0)
		return -1;

	return 0;
}

static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
			     enum nvnc_button_mask button_mask)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);
	struct wayvnc* wayvnc = wv_client->server;

	if (!wv_client->pointer.pointer) {
		return;
	}

	uint32_t xfx = 0, xfy = 0;
	output_transform_coord(wayvnc->selected_output, x, y, &xfx, &xfy);

	pointer_set(&wv_client->pointer, xfx, xfy, button_mask);
}

static void on_key_event(struct nvnc_client* client, uint32_t symbol,
                         bool is_pressed)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);
	if (!wv_client->keyboard.virtual_keyboard) {
		return;
	}

	keyboard_feed(&wv_client->keyboard, symbol, is_pressed);

	nvnc_client_set_led_state(wv_client->nvnc_client,
			keyboard_get_led_state(&wv_client->keyboard));
}

static void on_key_code_event(struct nvnc_client* client, uint32_t code,
		bool is_pressed)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);
	if (!wv_client->keyboard.virtual_keyboard) {
		return;
	}

	keyboard_feed_code(&wv_client->keyboard, code + 8, is_pressed);

	nvnc_client_set_led_state(wv_client->nvnc_client,
			keyboard_get_led_state(&wv_client->keyboard));
}

static void on_client_cut_text(struct nvnc_client* nvnc_client,
		const char* text, uint32_t len)
{
	struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);

	if (client->data_control.manager) {
		data_control_to_clipboard(&client->data_control, text, len);
	}
}

static bool on_client_resize(struct nvnc_client* nvnc_client,
		const struct nvnc_desktop_layout* layout)
{
	struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);
	struct wayvnc* self = client->server;

	uint16_t width = nvnc_desktop_layout_get_width(layout);
	uint16_t height = nvnc_desktop_layout_get_height(layout);
	struct output* output = client->server->selected_output;

	if (output == NULL)
		return false;

	if (self->master_layout_client && self->master_layout_client != client)
		return false;

	self->master_layout_client = client;

	nvnc_log(NVNC_LOG_DEBUG,
		"Client resolution changed: %ux%u, capturing output %s which is headless: %s",
		width, height, output->name,
		output->is_headless ? "yes" : "no");

	return wlr_output_manager_resize_output(output, width, height);
}

bool on_auth(const char* username, const char* password, void* ud)
{
	struct wayvnc* self = ud;

#ifdef ENABLE_PAM
	if (self->cfg.enable_pam)
		return pam_auth(username, password);
#endif

	if (strcmp(username, self->cfg.username) != 0)
		return false;

	if (strcmp(password, self->cfg.password) != 0)
		return false;
	return true;
}

static struct nvnc_fb* create_placeholder_buffer(uint16_t width, uint16_t height)
{
	uint16_t stride = width;
	struct nvnc_fb* fb = nvnc_fb_new(width, height, DRM_FORMAT_XRGB8888,
			stride);
	if (!fb)
		return NULL;

	size_t size = nvnc_fb_get_pixel_size(fb) * height * stride;
	memset(nvnc_fb_get_addr(fb), 0x60, size);

	return fb;
}

static int blank_screen(struct wayvnc* self)
{
	int width = 1280;
	int height = 720;

	if (self->selected_output) {
		width = output_get_transformed_width(self->selected_output);
		height = output_get_transformed_height(self->selected_output);
	}

	struct nvnc_fb* placeholder_fb = create_placeholder_buffer(width, height);
	if (!placeholder_fb) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to allocate a placeholder buffer");
		return -1;
	}

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0,
			nvnc_fb_get_width(placeholder_fb),
			nvnc_fb_get_height(placeholder_fb));

	nvnc_display_feed_buffer(self->nvnc_display, placeholder_fb, &damage);
	pixman_region_fini(&damage);
	nvnc_fb_unref(placeholder_fb);
	return 0;
}

static char* get_cfg_path(const struct cfg* cfg, char* dst, const char* src)
{
	if (!cfg->use_relative_paths || src[0] == '/') {
		strlcpy(dst, src, PATH_MAX);
		return dst;
	}

	snprintf(dst, PATH_MAX, "%s/%s", cfg->directory, src);
	return dst;
}

static bool is_prefix(const char* prefix, const char* str)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

static int count_colons_in_string(const char* str)
{
	int n = 0;
	for (int i = 0; str[i]; ++i)
		if (str[i] == ':')
			++n;
	return n;
}

static char* parse_address_prefix(char* addr, enum socket_type* socket_type)
{
	if (is_prefix("tcp:", addr)) {
		*socket_type = SOCKET_TYPE_TCP;
		addr += 4;
	} else if (is_prefix("unix:", addr)) {
		*socket_type = SOCKET_TYPE_UNIX;
		addr += 5;
	} else if (is_prefix("ws:", addr)) {
		*socket_type = SOCKET_TYPE_WEBSOCKET;
		addr += 3;
	} else if (is_prefix("fd:", addr)) {
		*socket_type = SOCKET_TYPE_FROM_FD;
		addr += 3;
	}
	return addr;
}

static char* parse_address_port(char* addr, uint16_t* port)
{
	// IPv6 addresses with port need to look like: [::]:5900
	if (addr[0] == '[') {
		char* end = strchr(addr, ']');
		if (!end)
			return addr;

		*end++ = '\0';

		if (*end == ':')
			*port = atoi(end + 1);

		return addr + 1;
	}

	if (count_colons_in_string(addr) > 1) {
		// This is most likely IPv6, so let's leave it alone
		return addr;
	}

	char* p = NULL;
	p = strrchr(addr, ':');
	if (!p)
		return addr;

	*p++ = '\0';

	*port = atoi(p);

	return addr;
}

static int add_listening_address(struct wayvnc* self, const char* address,
		uint16_t port, enum socket_type socket_type)
{
	char buffer[256];
	char* addr = buffer;
	strlcpy(buffer, address, sizeof(buffer));

	addr = parse_address_prefix(addr, &socket_type);

	if (socket_type == SOCKET_TYPE_TCP ||
			socket_type == SOCKET_TYPE_WEBSOCKET) {
		uint16_t parsed_port = 0;
		addr = parse_address_port(addr, &parsed_port);
		if (parsed_port)
			port = parsed_port;
	}

	int rc = -1;
	switch (socket_type) {
		case SOCKET_TYPE_TCP:
			rc = nvnc_listen_tcp(self->nvnc, addr, port,
					NVNC_STREAM_NORMAL);
			break;
		case SOCKET_TYPE_UNIX:
			rc = nvnc_listen_unix(self->nvnc, addr,
					NVNC_STREAM_NORMAL);
			break;
		case SOCKET_TYPE_WEBSOCKET:
			rc = nvnc_listen_tcp(self->nvnc, addr, port,
					NVNC_STREAM_WEBSOCKET);
			break;
		case SOCKET_TYPE_FROM_FD:;
			int fd = atoi(addr);
			rc = nvnc_listen(self->nvnc, fd, NVNC_STREAM_NORMAL);
			break;
		default:
			abort();
	}

	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to listen on socket or bind to its address. Add -Ldebug to the argument list for more info.");
		return -1;
	}
	if (socket_type == SOCKET_TYPE_UNIX)
		nvnc_log(NVNC_LOG_INFO, "Listening for connections on %s", addr);
	else if (socket_type == SOCKET_TYPE_FROM_FD)
		nvnc_log(NVNC_LOG_INFO, "Listening for connections on fd %s",
				addr);
	else
		nvnc_log(NVNC_LOG_INFO, "Listening for connections on %s:%d",
				addr, port);

	return 0;
}

static int apply_addresses_from_config(struct wayvnc* self,
		enum socket_type default_socket_type)
{
	uint16_t port = self->cfg.port ? self->cfg.port : DEFAULT_PORT;

	if (!self->cfg.address)
		return add_listening_address(self, DEFAULT_ADDRESS, port,
					default_socket_type);

	char *addresses = strdup(self->cfg.address);
	assert(addresses);

	int rc = -1;

	const char* delim = " ";
	char* tok = strtok(addresses, delim);
	while (tok) {
		if (add_listening_address(self, tok, port, default_socket_type) < 0)
			goto out;
		tok = strtok(NULL, delim);
	}

	rc = 0;
out:
	free(addresses);
	return rc;
}

static int init_nvnc(struct wayvnc* self)
{
	self->nvnc = nvnc_new();
	if (!self->nvnc)
		return -1;

	self->nvnc_display = nvnc_display_new(0, 0);
	if (!self->nvnc_display)
		goto display_failure;

	nvnc_add_display(self->nvnc, self->nvnc_display);

	nvnc_set_userdata(self->nvnc, self, NULL);

	nvnc_set_name(self->nvnc, "WayVNC");

	if (self->enable_resizing)
		nvnc_set_desktop_layout_fn(self->nvnc, on_client_resize);

	enum nvnc_auth_flags auth_flags = 0;
	if (self->cfg.enable_auth) {
		auth_flags |= NVNC_AUTH_REQUIRE_AUTH;
	}
	if (!self->cfg.relax_encryption) {
		auth_flags |= NVNC_AUTH_REQUIRE_ENCRYPTION;
	}

	if (self->cfg.enable_auth) {
		if (nvnc_enable_auth(self->nvnc, auth_flags, on_auth, self) < 0) {
			nvnc_log(NVNC_LOG_ERROR, "Failed to enable authentication");
			goto auth_failure;
		}

		if (self->cfg.rsa_private_key_file) {
			char tmp[PATH_MAX];
			const char* key_file = get_cfg_path(&self->cfg, tmp,
					self->cfg.rsa_private_key_file);
			if (nvnc_set_rsa_creds(self->nvnc, key_file) < 0) {
				nvnc_log(NVNC_LOG_ERROR, "Failed to load RSA credentials");
				goto auth_failure;
			}
		}

		if (self->cfg.private_key_file) {
			char key_file[PATH_MAX];
			char cert_file[PATH_MAX];

			get_cfg_path(&self->cfg, key_file,
					self->cfg.private_key_file);
			get_cfg_path(&self->cfg, cert_file,
					self->cfg.certificate_file);
			int r = nvnc_set_tls_creds(self->nvnc, key_file,
					cert_file);
			if (r < 0) {
				nvnc_log(NVNC_LOG_ERROR, "Failed to enable TLS authentication");
				goto auth_failure;
			}
		}
	}

	nvnc_set_pointer_fn(self->nvnc, on_pointer_event);

	nvnc_set_key_fn(self->nvnc, on_key_event);
	nvnc_set_key_code_fn(self->nvnc, on_key_code_event);

	nvnc_set_new_client_fn(self->nvnc, on_nvnc_client_new);
	nvnc_set_cut_text_fn(self->nvnc, on_client_cut_text);

	if (blank_screen(self) != 0)
		goto blank_screen_failure;

	return 0;

blank_screen_failure:
auth_failure:
	nvnc_display_unref(self->nvnc_display);
display_failure:
	nvnc_del(self->nvnc);
	return -1;
}

static void wayvnc_start_cursor_capture(struct wayvnc* self, bool immediate)
{
	if (self->cursor_sc) {
		screencopy_start(self->cursor_sc, immediate);
	}
}

int wayvnc_start_capture(struct wayvnc* self)
{
	int rc = screencopy_start(self->screencopy, false);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to start capture. Exiting...");
		wayvnc_exit(self);
	}
	return rc;
}

int wayvnc_start_capture_immediate(struct wayvnc* self)
{
	if (self->capture_retry_timer)
		return 0;

	struct output* output = self->selected_output;
	int rc = output_acquire_power_on(output);
	if (rc == 0) {
		nvnc_log(NVNC_LOG_DEBUG, "Acquired power state management. Waiting for power event to start capturing");
		return 0;
	} else if (rc > 0 && output->power != OUTPUT_POWER_ON) {
		nvnc_log(NVNC_LOG_DEBUG, "Output power state management already acquired, but not yet powered on");
		return 0;
	} else if (rc < 0) {
		nvnc_log(NVNC_LOG_WARNING, "Failed to acquire power state control. Capturing may fail.");
	}

	rc = screencopy_start(self->screencopy, true);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to start capture. Exiting...");
		wayvnc_exit(self);
	}
	return rc;
}

static void on_capture_restart_timer(struct aml_timer* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);
	aml_unref(self->capture_retry_timer);
	self->capture_retry_timer = NULL;
	wayvnc_start_capture_immediate(self);
}

static void wayvnc_restart_capture(struct wayvnc* self)
{
	if (self->capture_retry_timer)
		return;

	int timeout = 100000;
	self->capture_retry_timer = aml_timer_new(timeout,
			on_capture_restart_timer, self, NULL);
	aml_start(aml_get_default(), self->capture_retry_timer);
}

// TODO: Handle transform change too
void on_output_dimension_change(struct output* output)
{
	struct wayvnc* self = output->userdata;
	assert(self->selected_output == output);

	if (self->nr_clients == 0)
		return;

	nvnc_log(NVNC_LOG_DEBUG, "Output dimensions changed. Restarting frame capturer...");

	screencopy_stop(self->screencopy);
	wayvnc_start_capture_immediate(self);
}

static void on_output_power_change(struct output* output)
{
	nvnc_trace("Output %s power state changed to %s", output->name,
			output_power_state_name(output->power));

	struct wayvnc* self = output->userdata;
	if (self->selected_output != output || self->nr_clients == 0)
		return;

	switch (output->power) {
	case OUTPUT_POWER_ON:
		wayvnc_start_capture_immediate(self);
		wayvnc_start_cursor_capture(self, true);
		break;
	case OUTPUT_POWER_OFF:
		nvnc_log(NVNC_LOG_WARNING, "Output is now off. Pausing frame capture");
		screencopy_stop(self->cursor_sc);
		screencopy_stop(self->screencopy);
		blank_screen(self);
		break;
	default:
		break;
	}
}

static void apply_output_transform(const struct wayvnc* self,
		struct wv_buffer* buffer, struct pixman_region16* damage)
{
	enum wl_output_transform output_transform, buffer_transform;
	output_transform = self->selected_output->transform;

	if (buffer->y_inverted) {
		buffer_transform = wv_output_transform_compose(output_transform,
				WL_OUTPUT_TRANSFORM_FLIPPED_180);

		wv_region_transform(damage, &buffer->frame_damage,
				WL_OUTPUT_TRANSFORM_FLIPPED_180,
				buffer->width, buffer->height);
	} else {
		buffer_transform = output_transform;
		pixman_region_copy(damage, &buffer->frame_damage);
	}

	nvnc_fb_set_transform(buffer->nvnc_fb,
			(enum nvnc_transform)buffer_transform);
}

static void wayvnc_send_next_frame(struct wayvnc* self, uint64_t now)
{
	struct pixman_region16 damage;
	pixman_region_init(&damage);

	struct wv_buffer* buffer = self->next_frame;
	self->next_frame = NULL;

	if (self->screencopy->impl->caps & SCREENCOPY_CAP_TRANSFORM) {
		pixman_region_copy(&damage, &buffer->frame_damage);
	} else {
		apply_output_transform(self, buffer, &damage);
	}

	pixman_region_intersect_rect(&damage, &damage, 0, 0, buffer->width,
			buffer->height);

	nvnc_display_feed_buffer(self->nvnc_display, buffer->nvnc_fb,
			&damage);
	self->n_frames_sent++;

	pixman_region_fini(&damage);

	wayvnc_start_capture(self);

	nvnc_fb_unref(buffer->nvnc_fb);

	self->last_send_time = now;
}

static void wayvnc_handle_rate_limit_timeout(struct aml_timer* timer)
{
	struct wayvnc* self = aml_get_userdata(timer);
	uint64_t now = gettime_us();
	assert(self->next_frame);
	wayvnc_send_next_frame(self, now);
}

static void wayvnc_process_frame(struct wayvnc* self, struct wv_buffer* buffer)
{
	nvnc_trace("Processing buffer: %p", buffer);

	self->n_frames_captured++;
	self->damage_area_sum += calculate_region_area(&buffer->frame_damage);

	bool have_pending_frame = false;
	if (self->next_frame) {
		pixman_region_union(&buffer->frame_damage,
				&buffer->frame_damage,
				&self->next_frame->frame_damage);
		nvnc_fb_unref(self->next_frame->nvnc_fb);
		have_pending_frame = true;
	}
	self->next_frame = buffer;
	nvnc_fb_ref(buffer->nvnc_fb);

	if (have_pending_frame)
		return;

	uint64_t now = gettime_us();
	double dt = (now - self->last_send_time) * 1.0e-6;
	int32_t time_left = (1.0 / self->max_rate - dt) * 1.0e6;

	if (time_left > 0) {
		aml_set_duration(self->rate_limiter, time_left);
		aml_start(aml_get_default(), self->rate_limiter);
	} else {
		wayvnc_send_next_frame(self, now);
	}
}

void on_capture_done(enum screencopy_result result, struct wv_buffer* buffer,
		void* userdata)
{
	struct wayvnc* self = userdata;

	switch (result) {
	case SCREENCOPY_FATAL:
		nvnc_log(NVNC_LOG_ERROR, "Fatal error while capturing. Exiting...");
		wayvnc_exit(self);
		break;
	case SCREENCOPY_FAILED:
		wayvnc_restart_capture(self);
		break;
	case SCREENCOPY_DONE:
		wayvnc_process_frame(self, buffer);
		break;
	}
}

int wayvnc_usage(struct option_parser* parser, FILE* stream, int rc)
{
	fprintf(stream, "Usage: wayvnc");
	option_parser_print_usage(parser, stream);
	fprintf(stream, "\n");
	option_parser_print_cmd_summary("Starts a VNC server for $WAYLAND_DISPLAY",
			stream);
	if (option_parser_print_arguments(parser, stream))
		fprintf(stream, "\n");
	option_parser_print_options(parser, stream);
	fprintf(stream, "\n");
	return rc;
}

int check_cfg_sanity(struct cfg* cfg)
{
	if (cfg->enable_auth) {
		int rc = 0;

		if (!nvnc_has_auth()) {
			nvnc_log(NVNC_LOG_ERROR, "Authentication can't be enabled because it was not selected during build");
			rc = -1;
		}

		if (!!cfg->certificate_file != !!cfg->private_key_file) {
			nvnc_log(NVNC_LOG_ERROR, "Need both certificate_file and private_key_file for TLS");
			rc = -1;
		}

		if (!cfg->username && !cfg->enable_pam) {
			nvnc_log(NVNC_LOG_ERROR, "Authentication enabled, but missing username");
			rc = -1;
		}

		if (!cfg->password && !cfg->enable_pam) {
			nvnc_log(NVNC_LOG_ERROR, "Authentication enabled, but missing password");
			rc = -1;
		}

		if (cfg->relax_encryption) {
			nvnc_log(NVNC_LOG_WARNING, "Authentication enabled with relaxed encryption; not all sessions are guaranteed to be encrypted");
		}

		return rc;
	}

	return 0;
}

static void on_perf_tick(struct aml_ticker* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);

	double total_area = self->selected_output->width * self->selected_output->height;
	double area_avg = (double)self->damage_area_sum / (double)self->n_frames_captured;
	double relative_area_avg = 100.0 * area_avg / total_area;

	nvnc_log(NVNC_LOG_INFO, "Frames captured: %"PRIu32", frames sent: %"PRIu32" average reported frame damage: %.1f %%",
			self->n_frames_captured, self->n_frames_sent, relative_area_avg);

	self->n_frames_captured = 0;
	self->n_frames_sent = 0;
	self->damage_area_sum = 0;
}

static void start_performance_ticker(struct wayvnc* self)
{
	if (!self->performance_ticker)
		return;

	aml_start(aml_get_default(), self->performance_ticker);
}

static void stop_performance_ticker(struct wayvnc* self)
{
	if (!self->performance_ticker)
		return;

	aml_stop(aml_get_default(), self->performance_ticker);
}

static void client_init_wayland(struct wayvnc_client* self)
{
	client_init_seat(self);
	client_init_keyboard(self);
	client_init_pointer(self);
	client_init_data_control(self);
}

static void client_detach_wayland(struct wayvnc_client* self)
{
	self->seat = NULL;

	if (self->keyboard.virtual_keyboard) {
		zwp_virtual_keyboard_v1_destroy(
				self->keyboard.virtual_keyboard);
		keyboard_destroy(&self->keyboard);
	}
	self->keyboard.virtual_keyboard = NULL;

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);
	self->pointer.pointer = NULL;

	if (self->data_control.manager)
		data_control_destroy(&self->data_control);
	self->data_control.manager = NULL;
}

static unsigned next_client_id = 1;

static struct wayvnc_client* client_create(struct wayvnc* wayvnc,
		struct nvnc_client* nvnc_client)
{
	struct wayvnc_client* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->server = wayvnc;
	self->nvnc_client = nvnc_client;

	self->id = next_client_id++;

	if (!wayvnc->cursor_master)
		wayvnc->cursor_master = self;

	if (wayvnc->display) {
		client_init_wayland(self);
	}

	return self;
}

static void client_destroy(void* obj)
{
	struct wayvnc_client* self = obj;
	struct nvnc* nvnc = nvnc_client_get_server(self->nvnc_client);
	struct wayvnc* wayvnc = nvnc_get_userdata(nvnc);

	if (self == wayvnc->master_layout_client)
		wayvnc->master_layout_client = NULL;

	if (self == wayvnc->cursor_master) {
		nvnc_set_cursor(wayvnc->nvnc, NULL, 0, 0, 0, 0, false);
		screencopy_stop(wayvnc->cursor_sc);
		screencopy_destroy(wayvnc->cursor_sc);
		wayvnc->cursor_sc = NULL;
		wayvnc->cursor_master = NULL;
	}

	if (self->transient_seat)
		ext_transient_seat_v1_destroy(self->transient_seat);

	if (self->seat)
		self->seat->occupancy--;

	wayvnc->nr_clients--;
	nvnc_log(NVNC_LOG_DEBUG, "Client disconnected, new client count: %d",
			wayvnc->nr_clients);

	if (wayvnc->ctl) {
		struct ctl_server_client_info info = {};
		compose_client_info(self, &info);

		ctl_server_event_disconnected(wayvnc->ctl, &info,
				wayvnc->nr_clients);
	}

	if (wayvnc->nr_clients == 0 && wayvnc->display) {
		nvnc_log(NVNC_LOG_INFO, "Stopping screen capture");
		screencopy_stop(wayvnc->screencopy);
		output_release_power_on(wayvnc->selected_output);
		stop_performance_ticker(wayvnc);
	}

	if (self->keyboard.virtual_keyboard) {
		zwp_virtual_keyboard_v1_destroy(
				self->keyboard.virtual_keyboard);
		keyboard_destroy(&self->keyboard);
	}

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);

	if (self->data_control.manager)
		data_control_destroy(&self->data_control);

	free(self);
}

static void handle_first_client(struct wayvnc* self)
{
	nvnc_log(NVNC_LOG_INFO, "Starting screen capture");
	start_performance_ticker(self);
	wayvnc_start_capture_immediate(self);
}

static void on_nvnc_client_new(struct nvnc_client* client)
{
	struct nvnc* nvnc = nvnc_client_get_server(client);
	struct wayvnc* self = nvnc_get_userdata(nvnc);

	struct wayvnc_client* wayvnc_client = client_create(self, client);
	assert(wayvnc_client);
	nvnc_set_userdata(client, wayvnc_client, client_destroy);

	if (self->nr_clients++ == 0 && self->display) {
		handle_first_client(self);
	}
	nvnc_log(NVNC_LOG_DEBUG, "Client connected, new client count: %d",
			self->nr_clients);

	struct ctl_server_client_info info = {};
	compose_client_info(wayvnc_client, &info);

	ctl_server_event_connected(self->ctl, &info, self->nr_clients);
}

void parse_keyboard_option(struct wayvnc* self, const char* arg)
{
	// Find optional variant, separated by -
	char* index = strchr(arg, '-');
	if (index != NULL) {
		self->kb_variant = index + 1;
		// layout needs to be 0-terminated, replace the - by 0
		*index = 0;
	}
	self->kb_layout = arg;
}

static void client_init_pointer(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (!wayvnc->pointer_manager)
		return;

	self->pointer.vnc = self->server->nvnc;
	self->pointer.output = self->server->selected_output;

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);

	int pointer_manager_version =
		zwlr_virtual_pointer_manager_v1_get_version(wayvnc->pointer_manager);

	self->pointer.pointer = pointer_manager_version >= 2
		? zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
			wayvnc->pointer_manager, self->seat->wl_seat,
			wayvnc->selected_output->wl_output)
		: zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
			wayvnc->pointer_manager, self->seat->wl_seat);

	if (pointer_init(&self->pointer) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise pointer");
	}

	if (self == wayvnc->cursor_master) {
		// Get seat capability update
		// TODO: Make this asynchronous
		wl_display_roundtrip(wayvnc->display);
		wl_display_dispatch_pending(wayvnc->display);

		configure_cursor_sc(wayvnc, self);
		if (wayvnc->cursor_sc)
			screencopy_start(wayvnc->cursor_sc, true);
	}
}

static void handle_transient_seat_ready(void* data,
		struct ext_transient_seat_v1* transient_seat,
		uint32_t global_name)
{
	(void)transient_seat;

	struct wayvnc_client* client = data;
	struct wayvnc* wayvnc = client->server;

	struct seat* seat = seat_find_by_id(&wayvnc->seats, global_name);
	assert(seat);

	client->seat = seat;
}

static void handle_transient_seat_denied(void* data,
		struct ext_transient_seat_v1* transient_seat)
{
	(void)data;
	(void)transient_seat;

	// TODO: Something more graceful perhaps?
	nvnc_log(NVNC_LOG_PANIC, "Transient seat denied");
}

static void client_init_transient_seat(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	self->transient_seat =
		ext_transient_seat_manager_v1_create(wayvnc->transient_seat_manager);

	static const struct ext_transient_seat_v1_listener listener = {
		.ready = handle_transient_seat_ready,
		.denied = handle_transient_seat_denied,
	};
	ext_transient_seat_v1_add_listener(self->transient_seat, &listener,
			self);

	// TODO: Make this asynchronous
	wl_display_roundtrip(wayvnc->display);

	assert(self->seat);
}

static void client_init_seat(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (wayvnc->disable_input)
		return;

	if (wayvnc->selected_seat) {
		self->seat = wayvnc->selected_seat;
	} else if (wayvnc->use_transient_seat) {
		client_init_transient_seat(self);
	} else {
		self->seat = seat_find_unoccupied(&wayvnc->seats);
		if (!self->seat) {
			self->seat = seat_first(&wayvnc->seats);
		}
	}

	if (self->seat)
		self->seat->occupancy++;
}

static void client_init_keyboard(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (!wayvnc->keyboard_manager)
		return;

	self->keyboard.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			wayvnc->keyboard_manager, self->seat->wl_seat);

	struct xkb_rule_names rule_names = {
		.rules = wayvnc->cfg.xkb_rules,
		.layout = wayvnc->kb_layout ? wayvnc->kb_layout
			: wayvnc->cfg.xkb_layout,
		.model = wayvnc->cfg.xkb_model ? wayvnc->cfg.xkb_model
			: "pc105",
		.variant = wayvnc->kb_variant ? wayvnc->kb_variant :
			wayvnc->cfg.xkb_variant,
		.options = wayvnc->cfg.xkb_options,
	};

	if (keyboard_init(&self->keyboard, &rule_names) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise keyboard");
	}
}

static void reinitialise_pointers(struct wayvnc* self)
{
	struct nvnc_client* c;
	for (c = nvnc_client_first(self->nvnc); c; c = nvnc_client_next(c)) {
		struct wayvnc_client* client = nvnc_get_userdata(c);
		client_init_pointer(client);
	}
}

static void client_init_data_control(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (!wayvnc->data_control_manager)
		return;

	self->data_control.manager = wayvnc->data_control_manager;
	data_control_init(&self->data_control, wayvnc->nvnc,
			self->seat->wl_seat);
}

void log_selected_output(struct wayvnc* self)
{
	nvnc_log(NVNC_LOG_INFO, "Capturing output %s",
			self->selected_output->name);
	struct output* output;
	wl_list_for_each(output, &self->outputs, link) {
		bool this_output = (output->id == self->selected_output->id);
		nvnc_log(NVNC_LOG_INFO, "%s %s %dx%d+%dx%d Power:%s",
				this_output ? ">>" : "--",
				output->description,
				output->width, output->height,
				output->x, output->y,
				output_power_state_name(output->power));
	}
}

static void wayvnc_process_cursor(struct wayvnc* self, struct wv_buffer* buffer)
{
	nvnc_log(NVNC_LOG_DEBUG, "Got new cursor");
	bool is_damaged = pixman_region_not_empty(&buffer->frame_damage);
	nvnc_set_cursor(self->nvnc, buffer->nvnc_fb, buffer->width,
			buffer->height, buffer->x_hotspot, buffer->y_hotspot,
			is_damaged);
	wayvnc_start_cursor_capture(self, false);
}

static void on_cursor_capture_done(enum screencopy_result result,
		struct wv_buffer* buffer, void* userdata)
{
	struct wayvnc* self = userdata;

	switch (result) {
	case SCREENCOPY_FATAL:
		nvnc_log(NVNC_LOG_ERROR, "Fatal error while capturing. Exiting...");
		wayvnc_exit(self);
		break;
	case SCREENCOPY_FAILED:
		wayvnc_start_cursor_capture(self, true);
		break;
	case SCREENCOPY_DONE:
		wayvnc_process_cursor(self, buffer);
		break;
	}
}

static double rate_format(const void* userdata, enum wv_buffer_type type,
		enum wv_buffer_domain domain, uint32_t format,
		uint64_t modifier)
{
	const struct wayvnc* self = userdata;

	enum nvnc_fb_type fb_type = NVNC_FB_UNSPEC;
	switch (type) {
	case WV_BUFFER_SHM:
		fb_type = NVNC_FB_SIMPLE;
		break;
#ifdef ENABLE_SCREENCOPY_DMABUF
	case WV_BUFFER_DMABUF:
		fb_type = NVNC_FB_GBM_BO;
		break;
#endif
	case WV_BUFFER_UNSPEC:;
	}
	assert(fb_type != NVNC_FB_UNSPEC);

	switch (domain) {
	case WV_BUFFER_DOMAIN_OUTPUT:
		return nvnc_rate_pixel_format(self->nvnc, fb_type, format,
				modifier);
	case WV_BUFFER_DOMAIN_CURSOR:
		return nvnc_rate_cursor_pixel_format(self->nvnc, fb_type, format,
				modifier);
	case WV_BUFFER_DOMAIN_UNSPEC:;
	}

	abort();
	return -1;
}

static bool configure_cursor_sc(struct wayvnc* self,
		struct wayvnc_client* client)
{
	nvnc_log(NVNC_LOG_DEBUG, "Configuring cursor capturing");

	screencopy_stop(self->cursor_sc);
	screencopy_destroy(self->cursor_sc);

	struct seat* seat = client->seat;
	assert(seat);

	if (!(seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		nvnc_log(NVNC_LOG_DEBUG, "Client's seat has no pointer capability");
		return false;
	}

	self->cursor_sc = screencopy_create_cursor(
			self->selected_output->wl_output, seat->wl_seat);
	if (!self->cursor_sc) {
		nvnc_log(NVNC_LOG_DEBUG, "Failed to capture cursor");
		return false;
	}

	self->cursor_sc->on_done = on_cursor_capture_done;
	self->cursor_sc->rate_format = rate_format;
	self->cursor_sc->userdata = self;

	self->cursor_sc->rate_limit = self->max_rate * 2;
	self->cursor_sc->enable_linux_dmabuf = false;

	nvnc_log(NVNC_LOG_DEBUG, "Configured cursor capturing");
	return true;
}

bool configure_screencopy(struct wayvnc* self)
{
	screencopy_stop(self->screencopy);
	screencopy_destroy(self->screencopy);

	self->screencopy = screencopy_create(self->selected_output->wl_output,
			self->overlay_cursor);
	if (!self->screencopy) {
		nvnc_log(NVNC_LOG_ERROR, "screencopy is not supported by compositor");
		return false;
	}

	self->screencopy->on_done = on_capture_done;
	self->screencopy->rate_format = rate_format;
	self->screencopy->userdata = self;

	/* Because screencopy (at least the way it's implemented in wlroots),
	 * does not capture immediately, but rather schedules a frame to be
	 * captured on next output commit event, if we use the exact rate limit,
	 * we'll sometimes hit the frame before commit and sometimes after.
	 *
	 * This is why we multiply the capture rate limit by 2 here and have a
	 * secondary rate limiter for frames sent to VNC.
	 */
	self->screencopy->rate_limit = self->max_rate * 2;
	self->screencopy->enable_linux_dmabuf = self->enable_gpu_features;

	return true;
}

void set_selected_output(struct wayvnc* self, struct output* output)
{
	if (self->selected_output) {
		self->selected_output->on_dimension_change = NULL;
	}
	self->selected_output = output;
	output->on_dimension_change = on_output_dimension_change;
	output->on_power_change = on_output_power_change;
	output->userdata = self;

	if (self->ctl)
		ctl_server_event_capture_changed(self->ctl, output->name);
	log_selected_output(self);
}

void switch_to_output(struct wayvnc* self, struct output* output)
{
	if (self->selected_output == output) {
		nvnc_log(NVNC_LOG_INFO, "Already selected output %s",
				output->name);
		return;
	}
	screencopy_stop(self->screencopy);
	output_release_power_on(output);
	set_selected_output(self, output);
	configure_screencopy(self);
	reinitialise_pointers(self);
	if (self->nr_clients > 0)
		wayvnc_start_capture_immediate(self);
	screencopy_stop(self->cursor_sc);
	if (self->cursor_sc)
		screencopy_start(self->cursor_sc, true);
}

void switch_to_next_output(struct wayvnc* self)
{
	nvnc_log(NVNC_LOG_INFO, "Rotating to next output");
	struct output* next = output_cycle(&self->outputs,
			self->selected_output, OUTPUT_CYCLE_FORWARD);

	switch_to_output(self, next);
}

void switch_to_prev_output(struct wayvnc* self)
{
	nvnc_log(NVNC_LOG_INFO, "Rotating to previous output");
	struct output* prev = output_cycle(&self->outputs,
			self->selected_output, OUTPUT_CYCLE_REVERSE);
	switch_to_output(self, prev);
}

static char intercepted_error[256];

static void intercept_cmd_error(const struct nvnc_log_data* meta,
		const char* message)
{
	if (meta->level != NVNC_LOG_ERROR) {
		nvnc_default_logger(meta, message);
		return;
	}

	struct nvnc_log_data meta_override = *meta;
	meta_override.level = NVNC_LOG_DEBUG;
	nvnc_default_logger(&meta_override, message);

	size_t len = strlen(intercepted_error);
	if (len != 0 && len < sizeof(intercepted_error) - 2)
		intercepted_error[len++] = '\n';

	strlcpy(intercepted_error + len, message,
			sizeof(intercepted_error) - len);
}

static struct cmd_response* on_attach(struct ctl* ctl, const char* display)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	assert(self);

	memset(intercepted_error, 0, sizeof(intercepted_error));
	nvnc_set_log_fn_thread_local(intercept_cmd_error);

	// TODO: Add optional output argument
	bool ok = wayland_attach(self, display, NULL);

	nvnc_set_log_fn_thread_local(NULL);

	return ok ? cmd_ok() : cmd_failed("%s", intercepted_error);
}

static bool wayland_attach(struct wayvnc* self, const char* display,
		const char* output)
{
	if (self->display) {
		wayland_detach(self);
	}

	nvnc_log(NVNC_LOG_DEBUG, "Attaching to %s", display);

	if (init_wayland(self, display) < 0) {
		return false;
	}

	struct output* out;
	if (output) {
		out = output_find_by_name(&self->outputs, output);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No such output: %s", output);
			wayland_detach(self);
			return false;
		}
	} else {
		out = output_first(&self->outputs);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No output available");
			wayland_detach(self);
			return false;
		}
	}

	if (!screencopy_manager) {
		nvnc_log(NVNC_LOG_ERROR, "Attached display does not implement wlr-screencopy-v1");
		wayland_detach(self);
		return false;
	}
	set_selected_output(self, out);
	configure_screencopy(self);

	struct nvnc_client* nvnc_client;
	for (nvnc_client = nvnc_client_first(self->nvnc); nvnc_client;
			nvnc_client = nvnc_client_next(nvnc_client)) {
		struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);
		client_init_wayland(client);
	}

	nvnc_log(NVNC_LOG_INFO, "Attached to %s", display);

	if (self->nr_clients > 0) {
		handle_first_client(self);
	}

	return true;
}

static struct cmd_response* on_detach(struct ctl* ctl)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	assert(self);

	if (!self->display) {
		return cmd_failed("Not attached!");
	}

	wayland_detach(self);
	nvnc_log(NVNC_LOG_INFO, "Detached from wayland server");
	return cmd_ok();
}

static int log_level_from_string(const char* str)
{
	if (0 == strcmp(str, "quiet")) return NVNC_LOG_PANIC;
	if (0 == strcmp(str, "error")) return NVNC_LOG_ERROR;
	if (0 == strcmp(str, "warning")) return NVNC_LOG_WARNING;
	if (0 == strcmp(str, "info")) return NVNC_LOG_INFO;
	if (0 == strcmp(str, "debug")) return NVNC_LOG_DEBUG;
	if (0 == strcmp(str, "trace")) return NVNC_LOG_TRACE;
	return -1;
}

int show_version(void)
{
	printf("wayvnc: %s\n", wayvnc_version);
	printf("neatvnc: %s\n", nvnc_version);
	printf("aml: %s\n", aml_version);
	return 0;
}

int main(int argc, char* argv[])
{
	struct wayvnc self = { 0 };

	const char* cfg_file = NULL;
	bool enable_gpu_features = false;

	const char* address = NULL;
	int port = 0;
	bool use_external_fd = false;
	bool use_unix_socket = false;
	bool use_websocket = false;
	bool start_detached = false;

	const char* output_name = NULL;
	const char* seat_name = NULL;
	const char* socket_path = NULL;
	const char* keyboard_options = NULL;

	bool overlay_cursor = false;
	bool show_performance = false;
	int max_rate = 30;
	bool disable_input = false;
	bool use_transient_seat = false;

	const char* log_level_name = NULL;
	bool is_verbose = false;
	int log_level = NVNC_LOG_WARNING;

	static const struct wv_option opts[] = {
		{ .positional = "address",
		  .help = "An address to listen on.",
		  .default_ = "localhost",
		  .is_repeating = true },
		{ 'C', "config", "<path>",
		  "Select a config file." },
		{ 'd', "disable-input", NULL,
		  "Disable all remote input." },
		{ 'D', "detached", NULL,
		  "Start detached from a compositor." },
		{ 'f', "max-fps", "<fps>",
		  "Set rate limit.",
		  .default_ = "30" },
		{ 'g', "gpu", NULL,
		  "Enable features that need GPU." },
		{ 'h', "help", NULL,
		  "Get help (this text)." },
		{ 'k', "keyboard", "<layout>[-<variant>]",
		  "Select keyboard layout with an optional variant." },
		{ 'L', "log-level", "<level>",
		  "Set log level. The levels are: error, warning, info, debug trace and quiet.",
		  .default_ = "warning" },
		{ 'o', "output", "<name>",
		  "Select output to capture." },
		{ 'p', "show-performance", NULL,
		  "Show performance counters." },
		{ 'r', "render-cursor", NULL,
		  "Enable overlay cursor rendering." },
		{ 'R', "disable-resizing", NULL,
		  "Disable automatic resizing." },
		{ 's', "seat", "<name>",
		  "Select seat by name." },
		{ 'S', "socket", "<path>",
		  "Control socket path." },
		{ 't', "transient-seat", NULL,
		  "Use transient seat." },
		{ 'u', "unix-socket", NULL,
		  "Create unix domain socket." },
		{ 'v', "verbose", NULL,
		  "Be more verbose. Same as setting --log-level=info" },
		{ 'V', "version", NULL,
		  "Show version info." },
		{ 'w', "websocket", NULL,
		  "Create a websocket." },
		{ 'x', "external-listener-fd", NULL,
		  "The address is a pre-bound file descriptor.", },
		{}
	};

	struct option_parser option_parser;
	option_parser_init(&option_parser, opts);

	if (option_parser_parse(&option_parser, argc,
				(const char* const*)argv) < 0)
		return wayvnc_usage(&option_parser, stderr, 1);

	if (option_parser_get_value(&option_parser, "version")) {
		return show_version();
	}

	if (option_parser_get_value(&option_parser, "help")) {
		return wayvnc_usage(&option_parser, stdout, 0);
	}

	cfg_file = option_parser_get_value(&option_parser, "config");
	enable_gpu_features = !!option_parser_get_value(&option_parser, "gpu");
	output_name = option_parser_get_value(&option_parser, "output");
	seat_name = option_parser_get_value(&option_parser, "seat");
	socket_path = option_parser_get_value(&option_parser, "socket");
	overlay_cursor = !!option_parser_get_value(&option_parser, "render-cursor");
	show_performance = !!option_parser_get_value(&option_parser,
			"show-performance");
	use_unix_socket = !!option_parser_get_value(&option_parser, "unix-socket");
	use_websocket = !!option_parser_get_value(&option_parser, "websocket");
	use_external_fd = !!option_parser_get_value(&option_parser,
			"external-listener-fd");
	disable_input = !!option_parser_get_value(&option_parser, "disable-input");
	is_verbose = option_parser_get_value(&option_parser, "verbose");
	log_level_name = option_parser_get_value(&option_parser, "log-level");
	max_rate = atoi(option_parser_get_value(&option_parser, "max-fps"));
	use_transient_seat = !!option_parser_get_value(&option_parser,
				"transient-seat");
	start_detached = !!option_parser_get_value(&option_parser, "detached");
	self.enable_resizing = !option_parser_get_value(&option_parser,
			"disable-resizing");

	self.start_detached = start_detached;
	self.overlay_cursor = overlay_cursor;
	self.max_rate = max_rate;
	self.enable_gpu_features = enable_gpu_features;

	keyboard_options = option_parser_get_value(&option_parser, "keyboard");
	if (keyboard_options)
		parse_keyboard_option(&self, keyboard_options);

	log_level = log_level_from_string(log_level_name);
	if (log_level < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Invalid log level: %s", log_level_name);
		return 1;
	}

	if (is_verbose && log_level < NVNC_LOG_INFO)
		log_level = NVNC_LOG_INFO;

	nvnc_set_log_level(log_level);

	if (seat_name && disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "seat and disable-input are conflicting options");
		return 1;
	}

	int n_address_modifiers = use_unix_socket + use_websocket +
		use_external_fd;
	if (n_address_modifiers > 1) {
		nvnc_log(NVNC_LOG_ERROR, "Only one of the websocket, unix-socket or the external-listener-fd options may be set");
		return 1;
	}

	if (use_transient_seat && disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "transient-seat and disable-input are conflicting options");
		return 1;
	}

	if (seat_name && use_transient_seat) {
		nvnc_log(NVNC_LOG_ERROR, "transient-seat and seat are conflicting options");
		return 1;
	}

	errno = 0;
	int cfg_rc = cfg_load(&self.cfg, cfg_file);
	if (cfg_rc != 0 && (cfg_file || errno != ENOENT)) {
		if (cfg_rc > 0) {
			nvnc_log(NVNC_LOG_ERROR, "Failed to load config. Error on line %d",
			          cfg_rc);
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to load config. %m");
		}

		return 1;
	}

	if (check_cfg_sanity(&self.cfg) < 0)
		return 1;

	self.disable_input = disable_input;
	self.use_transient_seat = use_transient_seat;

	srand(time(NULL));

	signal(SIGPIPE, SIG_IGN);

	struct aml* aml = aml_new();
	if (!aml)
		goto failure;

	aml_set_default(aml);

	if (init_main_loop(&self) < 0)
		goto failure;

	self.rate_limiter = aml_timer_new(0, wayvnc_handle_rate_limit_timeout,
			&self, NULL);
	if (!self.rate_limiter)
		goto rate_limiter_failure;

	if (!start_detached) {
		if (init_wayland(&self, NULL) < 0) {
			nvnc_log(NVNC_LOG_ERROR, "Failed to initialise wayland");
			goto wayland_failure;
		}

		struct output* out;
		if (output_name) {
			out = output_find_by_name(&self.outputs, output_name);
			if (!out) {
				nvnc_log(NVNC_LOG_ERROR, "No such output");
				goto wayland_failure;
			}
		} else {
			out = output_first(&self.outputs);
			if (!out) {
				nvnc_log(NVNC_LOG_ERROR, "No output found");
				goto wayland_failure;
			}
		}
		set_selected_output(&self, out);

		struct seat* seat = NULL;
		if (seat_name) {
			seat = seat_find_by_name(&self.seats, seat_name);
			if (!seat) {
				nvnc_log(NVNC_LOG_ERROR, "No such seat");
				goto wayland_failure;
			}
		}

		self.selected_seat = seat;
	}

	enum socket_type default_socket_type = SOCKET_TYPE_TCP;
	if (use_unix_socket)
		default_socket_type = SOCKET_TYPE_UNIX;
	else if (use_websocket)
		default_socket_type = SOCKET_TYPE_WEBSOCKET;
	else if (use_external_fd)
		default_socket_type = SOCKET_TYPE_FROM_FD;

	if (!start_detached && !configure_screencopy(&self))
		goto screencopy_failure;

	if (show_performance)
		self.performance_ticker = aml_ticker_new(1000000, on_perf_tick,
				&self, NULL);

	const struct ctl_server_actions ctl_actions = {
		.userdata = &self,
		.on_attach = on_attach,
		.on_detach = on_detach,
		.on_output_cycle = on_output_cycle,
		.on_output_switch = on_output_switch,
		.client_next = client_next,
		.client_info = client_info,
		.get_output_list = get_output_list,
		.on_disconnect_client = on_disconnect_client,
		.on_wayvnc_exit = on_wayvnc_exit,
	};
	self.ctl = ctl_server_new(socket_path, &ctl_actions);
	if (!self.ctl)
		goto ctl_server_failure;

	if (init_nvnc(&self) < 0)
		goto nvnc_failure;

	address = option_parser_get_value_with_offset(&option_parser,
			"address", 0);

	// If the second address argument is a number, we will assume it to be a
	// port for historical reasons.
	const char* port_str = option_parser_get_value_with_offset(
			&option_parser, "address", 1);
	if (port_str) {
		char* endptr;
		port = strtoul(port_str, &endptr, 0);
		if (!port_str[0] || endptr[0])
			port = 0;
	}

	if (port) {
		if (add_listening_address(&self, address, port,
				default_socket_type) < 0)
			goto nvnc_failure;
	} else if (!option_parser_get_value_with_offset(&option_parser,
				"address", 0)) {
		if (apply_addresses_from_config(&self, default_socket_type) < 0)
			goto nvnc_failure;
	} else {
		port = self.cfg.port ? self.cfg.port : DEFAULT_PORT;
		for (int i = 0;; ++i) {
			const char* address;
			address = option_parser_get_value_with_offset(
					&option_parser, "address", i);
			if (!address)
				break;

			if (add_listening_address(&self, address, port,
					default_socket_type) < 0)
				goto nvnc_failure;
		}
	}

	if (self.display)
		wl_display_dispatch_pending(self.display);

	while (!self.do_exit) {
		if (self.display)
			wl_display_flush(self.display);

		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	nvnc_log(NVNC_LOG_INFO, "Exiting...");

	if (self.display)
		screencopy_stop(self.screencopy);

	ctl_server_destroy(self.ctl);
	self.ctl = NULL;

	nvnc_display_unref(self.nvnc_display);
	nvnc_del(self.nvnc);
	self.nvnc = NULL;
	wayland_detach(&self);

	if (zwp_linux_dmabuf)
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf);
	if (self.screencopy)
		screencopy_destroy(self.screencopy);
	aml_stop(aml, self.rate_limiter);
	aml_unref(self.rate_limiter);
	aml_unref(aml);

	cfg_destroy(&self.cfg);

	return 0;

nvnc_failure:
	ctl_server_destroy(self.ctl);
	self.ctl = NULL;
	nvnc_display_unref(self.nvnc_display);
	nvnc_del(self.nvnc);
	self.nvnc = NULL;
ctl_server_failure:
screencopy_failure:
	wayland_detach(&self);
wayland_failure:
rate_limiter_failure:
	aml_stop(aml, self.rate_limiter);
	aml_unref(self.rate_limiter);
	aml_unref(aml);
failure:
	cfg_destroy(&self.cfg);
	return 1;
}
