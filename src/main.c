/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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

#include "ext-transient-seat-v1.h"
#include "virtual-keyboard-unstable-v1.h"

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
#include "image-source.h"
#include "toplevel.h"
#include "observer.h"
#include "desktop.h"
#include "wayland.h"

#ifdef ENABLE_PAM
#include "pam_auth.h"
#endif

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

#define XSTR(x) STR(x)
#define STR(x) #x

struct wayvnc_client;

enum socket_type {
	SOCKET_TYPE_TCP = 0,
	SOCKET_TYPE_UNIX,
	SOCKET_TYPE_FROM_FD,
};

struct wayvnc_display {
	LIST_ENTRY(wayvnc_display) link;
	struct wayvnc* wayvnc;
	struct nvnc_display* nvnc_display;
	struct image_source* image_source;
	struct wv_buffer* next_frame;
	struct observer geometry_change_observer;
	struct observer destruction_observer;
	struct {
		bool is_set;
		int width, height;
		enum wl_output_transform transform;
	} last_frame_info;
};

LIST_HEAD(wayvnc_display_list, wayvnc_display);

struct wayvnc {
	bool do_exit;
	bool exit_on_disconnect;

	struct cfg cfg;

	struct image_source* image_source;
	const char* selected_seat_name;

	enum image_source_type image_source_type;
	char image_source_name[128];

	struct screencopy* screencopy;

	struct aml_signal* signal_handler;

	struct nvnc* nvnc;
	struct wayvnc_display_list wayvnc_displays;

	const char* desktop_name;

	const char* kb_layout;
	const char* kb_variant;

	uint32_t damage_area_sum;
	uint32_t n_frames_captured;
	uint32_t n_frames_sent;

	bool disable_input;
	bool use_transient_seat;
	bool use_toplevel;

	int nr_clients;
	struct aml_ticker* performance_ticker;

	struct aml_timer* capture_retry_timer;

	struct ctl* ctl;

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

	// wayland observers
	struct observer output_added_observer;
	struct observer output_removed_observer;
	struct observer seat_removed_observer;
	struct observer wayland_destroy_observer;

	// image source observers
	struct observer power_change_observer;
	struct observer destruction_observer;
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
		struct image_source* source, void* userdata);
static void on_cursor_capture_done(enum screencopy_result result,
		struct wv_buffer* buffer, struct image_source* source,
		void* userdata);
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
		enum image_source_type, const char* image_source_name);
static void wayland_detach(struct wayvnc* self);
static bool configure_cursor_sc(struct wayvnc* self,
		struct wayvnc_client* client);
static bool wayvnc_desktop_display_add(struct wayvnc* self,
		struct image_source* image_source);

struct wayland* wayland = NULL;

extern struct screencopy_impl wlr_screencopy_impl, ext_image_copy_capture_impl;

static void on_output_added(struct observer* observer, void* data)
{
	struct wayvnc* self = wl_container_of(observer, self,
			output_added_observer);
	struct output* output = data;
	ctl_server_event_output_added(self->ctl, output->name);

	if (image_source_is_desktop(self->image_source)) {
		wayvnc_desktop_display_add(self, &output->image_source);
	}
}

static void on_output_removed(struct observer* observer, void* data)
{
	struct wayvnc* self = wl_container_of(observer, self,
			output_removed_observer);
	struct output* out = data;

	if (image_source_is_output(self->image_source) &&
			out == output_from_image_source(self->image_source)) {
		nvnc_log(NVNC_LOG_WARNING, "Selected output %s went away",
				out->name);
		switch_to_prev_output(self);
	} else
		nvnc_log(NVNC_LOG_INFO, "Output %s went away", out->name);

	ctl_server_event_output_removed(self->ctl, out->name);

	if (image_source_is_output(self->image_source) &&
			out == output_from_image_source(self->image_source)) {
		if (self->start_detached) {
			nvnc_log(NVNC_LOG_WARNING, "No fallback outputs left. Detaching...");
			wayland_detach(self);
		} else {
			nvnc_log(NVNC_LOG_ERROR, "No fallback outputs left. Exiting...");
			wayvnc_exit(self);
		}
	}
}

static void on_seat_removed(struct observer* observer, void* data)
{
	struct wayvnc* self = wl_container_of(observer, self,
			seat_removed_observer);
	struct seat* seat = data;
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

static void wayvnc_display_detach(struct wayvnc_display* display)
{
	nvnc_trace("removing destruction observer");
	observer_deinit(&display->destruction_observer);
	nvnc_trace("removing geometry observer");
	observer_deinit(&display->geometry_change_observer);
	if (display->next_frame) {
		nvnc_fb_unref(display->next_frame->nvnc_fb);
		display->next_frame = NULL;
	}
	display->image_source = NULL;
}

static void wayvnc_display_list_detach(struct wayvnc_display_list* list)
{
	struct wayvnc_display* display;
	LIST_FOREACH(display, list, link)
		wayvnc_display_detach(display);
}

static void wayland_detach(struct wayvnc* self)
{
	wayland_destroy(wayland);
}

static void on_wayland_destroyed(struct observer* observer, void* data)
{
	struct wayvnc* self = wl_container_of(observer, self,
			wayland_destroy_observer);

	observer_deinit(&self->output_added_observer);
	observer_deinit(&self->output_removed_observer);
	observer_deinit(&self->seat_removed_observer);
	observer_deinit(&self->wayland_destroy_observer);

	observer_deinit(&self->power_change_observer);
	observer_deinit(&self->destruction_observer);

	wayvnc_display_list_detach(&self->wayvnc_displays);

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

	screencopy_stop(self->screencopy);
	screencopy_destroy(self->screencopy);
	self->screencopy = NULL;

	screencopy_stop(self->cursor_sc);
	screencopy_destroy(self->cursor_sc);
	self->cursor_sc = NULL;

	if (self->image_source && image_source_is_desktop(self->image_source))
		image_source_destroy(self->image_source);
	self->image_source = NULL;

	if (self->performance_ticker) {
		aml_stop(aml_get_default(), self->performance_ticker);
		aml_unref(self->performance_ticker);
	}
	self->performance_ticker = NULL;

	if (self->capture_retry_timer) {
		aml_stop(aml_get_default(), self->capture_retry_timer);
		aml_unref(self->capture_retry_timer);
	}
	self->capture_retry_timer = NULL;

	if (self->ctl)
		ctl_server_event_detached(self->ctl);

	if (!self->start_detached)
		wayvnc_exit(self);

	wayland = NULL;
}

static int init_wayland(struct wayvnc* self, const char* display)
{
	assert(!wayland);

	enum wayland_flags flags = 0;
	if (!self->disable_input)
		flags |= WAYLAND_FLAG_ENABLE_INPUT;
	if (self->use_transient_seat)
		flags |= WAYLAND_FLAG_ENABLE_TRANSIENT_SEAT;
	if (self->use_toplevel)
		flags |= WAYLAND_FLAG_ENABLE_TOPLEVEL_CAPTURE;

	wayland = wayland_connect(display, flags);
	if (!wayland)
		return -1;

	observer_init(&self->output_added_observer,
			&wayland->observable.output_added, on_output_added);
	observer_init(&self->output_removed_observer,
			&wayland->observable.output_removed, on_output_removed);
	observer_init(&self->seat_removed_observer,
			&wayland->observable.seat_removed, on_seat_removed);
	observer_init(&self->wayland_destroy_observer,
			&wayland->observable.destroyed, on_wayland_destroyed);

	if (!wayland->zwlr_virtual_pointer_manager_v1 && !self->disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "Virtual Pointer protocol not supported by compositor.");
		nvnc_log(NVNC_LOG_ERROR, "wayvnc may still work if started with --disable-input.");
		goto failure;
	}

	if (!wayland->zwp_virtual_keyboard_manager_v1 && !self->disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "Virtual Keyboard protocol not supported by compositor.");
		nvnc_log(NVNC_LOG_ERROR, "wayvnc may still work if started with --disable-input.");
		goto failure;
	}

	if (!wayland->zwlr_screencopy_manager_v1 &&
			!wayland->ext_image_copy_capture_manager_v1) {
		nvnc_log(NVNC_LOG_ERROR, "Screencopy protocol not supported by compositor. Exiting. Refer to FAQ section in man page.");
		goto failure;
	}

	if (!wayland->ext_transient_seat_manager_v1 && self->use_transient_seat) {
		nvnc_log(NVNC_LOG_ERROR, "Transient seat protocol not supported by compositor");
		goto failure;
	}

	bool have_toplevel_capture =
		wayland->ext_foreign_toplevel_list_v1 &&
		wayland->ext_foreign_toplevel_image_capture_source_manager_v1;
	if (self->use_toplevel && !have_toplevel_capture) {
		nvnc_log(NVNC_LOG_ERROR, "Toplevel capture is not supported by the compositor");
		goto failure;
	}

	return 0;

failure:
	wayland_destroy(wayland);
	wayland = NULL;
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
	if (!wayland)
		return cmd_failed("Not attached!");
	if (!image_source_is_output(self->image_source))
		return cmd_failed("Not capturing an output!");
	struct output* next = output_cycle(&wayland->outputs,
			output_from_image_source(self->image_source), direction);
	switch_to_output(self, next);
	return cmd_ok();
}

struct cmd_response* on_output_switch(struct ctl* ctl,
			const char* output_name)
{
	nvnc_log(NVNC_LOG_INFO, "ctl command: Switch to output \"%s\"", output_name);

	struct wayvnc* self = ctl_server_userdata(ctl);
	if (!wayland)
		return cmd_failed("Not attached!");
	if (!image_source_is_output(self->image_source))
		return cmd_failed("Not capturing an output!");
	if (!output_name || output_name[0] == '\0')
		return cmd_failed("Output name is required");
	struct output* output = output_find_by_name(&wayland->outputs,
			output_name);
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

static struct cmd_response* on_set_desktop_name(struct ctl* ctl,
		const char* name)
{
	nvnc_log(NVNC_LOG_INFO, "ctl command: Setting desktop name to \"%s\"", name);

	struct wayvnc* self = ctl_server_userdata(ctl);
	nvnc_set_name(self->nvnc, name);
	return cmd_ok();
}

static int get_output_list(struct ctl* ctl,
		struct ctl_server_output** outputs)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	if (!wayland) {
		*outputs = NULL;
		return 0;
	}
	int n = wl_list_length(&wayland->outputs);
	if (n == 0) {
		*outputs = NULL;
		return 0;
	}
	*outputs = calloc(n, sizeof(**outputs));
	struct output* output;
	struct ctl_server_output* item = *outputs;
	wl_list_for_each(output, &wayland->outputs, link) {
		strlcpy(item->name, output->name, sizeof(item->name));
		strlcpy(item->description, output->description,
				sizeof(item->description));
		item->height = output->height;
		item->width = output->width;
		if (image_source_is_output(self->image_source)) {
			struct output* source_output =
				output_from_image_source(self->image_source);
			item->captured = (output->id == source_output->id);
		}
		strlcpy(item->power, image_source_power_state_name(output->power),
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

static struct wayvnc_display *wayvnc_display_find_by_source(struct wayvnc* self,
		struct image_source *source)
{
	struct wayvnc_display* display;
	LIST_FOREACH(display, &self->wayvnc_displays, link)
		if (display->image_source == source)
			return display;
	return NULL;
}

static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
			     enum nvnc_button_mask button_mask)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);
	struct wayvnc* wayvnc = wv_client->server;

	if (!wv_client->pointer.pointer) {
		return;
	}

	enum wl_output_transform transform;
	int width = 0, height = 0;
	if (image_source_get_dimensions(wayvnc->image_source, &width, &height)) {
		transform = image_source_get_transform(wayvnc->image_source);
	} else {
		struct wayvnc_display* display = wayvnc_display_find_by_source(
				wayvnc, wayvnc->image_source);
		assert(display);

		if (display->last_frame_info.is_set) {
			width = display->last_frame_info.width;
			height = display->last_frame_info.height;
			transform = display->last_frame_info.transform;
		} else {
			transform = WL_OUTPUT_TRANSFORM_NORMAL;
		}
	}

	struct { int x, y; } xf = { x, y };
	wv_output_transform_canvas_point(transform, width, height, &xf.x, &xf.y);

	pointer_set(&wv_client->pointer, xf.x, xf.y, button_mask);
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

	if (!self->image_source)
		return false;

	if (image_source_is_desktop(self->image_source)) {
		if (output_first(&wayland->outputs) != output_last(&wayland->outputs))
			return false;
	} else if (!image_source_is_output(self->image_source))
		return false;

	if (self->master_layout_client && self->master_layout_client != client)
		return false;

	self->master_layout_client = client;

	struct output* output;
	if (image_source_is_output(client->server->image_source))
		output = output_from_image_source(client->server->image_source);
	else
		output = output_first(&wayland->outputs);

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

static int wayvnc_display_blank(struct wayvnc* self,
		struct wayvnc_display* display)
{
	int width = 1280;
	int height = 720;

	if (display->image_source) {
		if (image_source_get_transformed_dimensions(display->image_source,
				&width, &height)) {
		} else if (display->last_frame_info.is_set) {
			width = display->last_frame_info.width;
			height = display->last_frame_info.height;
		}
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

	nvnc_display_feed_buffer(display->nvnc_display, placeholder_fb, &damage);
	pixman_region_fini(&damage);
	nvnc_fb_unref(placeholder_fb);

	nvnc_set_cursor(self->nvnc, NULL, 0, 0, 0, 0, false);

	return 0;
}

static int blank_screen(struct wayvnc* self)
{
	struct wayvnc_display* display;
	LIST_FOREACH(display, &self->wayvnc_displays, link)
		wayvnc_display_blank(self, display);
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

static char* parse_address_prefix(char* addr, enum socket_type* socket_type,
		enum nvnc_stream_type* stream_type)
{
	if (is_prefix("tcp:", addr)) {
		*socket_type = SOCKET_TYPE_TCP;
		*stream_type = NVNC_STREAM_NORMAL;
		addr += 4;
	} else if (is_prefix("unix:", addr)) {
		*socket_type = SOCKET_TYPE_UNIX;
		*stream_type = NVNC_STREAM_NORMAL;
		addr += 5;
	} else if (is_prefix("fd:", addr)) {
		*socket_type = SOCKET_TYPE_FROM_FD;
		*stream_type = NVNC_STREAM_NORMAL;
		addr += 3;
	} else if (is_prefix("ws:", addr)) {
		*socket_type = SOCKET_TYPE_TCP;
		*stream_type = NVNC_STREAM_WEBSOCKET;
		addr += 3;
	} else if (is_prefix("ws-tcp:", addr)) {
		*socket_type = SOCKET_TYPE_TCP;
		*stream_type = NVNC_STREAM_WEBSOCKET;
		addr += 7;
	} else if (is_prefix("ws-unix:", addr)) {
		*socket_type = SOCKET_TYPE_UNIX;
		*stream_type = NVNC_STREAM_WEBSOCKET;
		addr += 8;
	} else if (is_prefix("ws-fd:", addr)) {
		*socket_type = SOCKET_TYPE_FROM_FD;
		*stream_type = NVNC_STREAM_WEBSOCKET;
		addr += 6;
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
		uint16_t port, enum socket_type socket_type,
		enum nvnc_stream_type stream_type)
{
	char buffer[256];
	char* addr = buffer;
	strlcpy(buffer, address, sizeof(buffer));

	addr = parse_address_prefix(addr, &socket_type, &stream_type);

	if (socket_type == SOCKET_TYPE_TCP) {
		uint16_t parsed_port = 0;
		addr = parse_address_port(addr, &parsed_port);
		if (parsed_port)
			port = parsed_port;
	}

	int rc = -1;
	switch (socket_type) {
		case SOCKET_TYPE_TCP:
			rc = nvnc_listen_tcp(self->nvnc, addr, port,
					stream_type);
			break;
		case SOCKET_TYPE_UNIX:
			rc = nvnc_listen_unix(self->nvnc, addr, stream_type);
			break;
		case SOCKET_TYPE_FROM_FD:;
			int fd = atoi(addr);
			rc = nvnc_listen(self->nvnc, fd, stream_type);
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
		enum socket_type default_socket_type,
		enum nvnc_stream_type default_stream_type)
{
	uint16_t port = self->cfg.port ? self->cfg.port : DEFAULT_PORT;

	if (!self->cfg.address)
		return add_listening_address(self, DEFAULT_ADDRESS, port,
					default_socket_type,
					default_stream_type);

	char *addresses = strdup(self->cfg.address);
	assert(addresses);

	int rc = -1;

	const char* delim = " ";
	char* tok = strtok(addresses, delim);
	while (tok) {
		if (add_listening_address(self, tok, port, default_socket_type,
					default_stream_type) < 0)
			goto out;
		tok = strtok(NULL, delim);
	}

	rc = 0;
out:
	free(addresses);
	return rc;
}

static struct wayvnc_display* wayvnc_display_add(struct wayvnc* self,
		struct image_source* image_source, uint16_t x, uint16_t y)
{
	struct wayvnc_display* display = calloc(1, sizeof(*display));
	if (!display)
		return NULL;

	LIST_INSERT_HEAD(&self->wayvnc_displays, display, link);

	display->wayvnc = self;
	display->image_source = image_source;

	nvnc_log(NVNC_LOG_DEBUG, "Adding display at %d, %d", (int)x, (int)y);

	display->nvnc_display = nvnc_display_new(x, y);
	if (!display->nvnc_display) {
		free(display);
		return NULL;
	}

	nvnc_add_display(self->nvnc, display->nvnc_display);

	return display;
}

static void wayvnc_display_destroy(struct wayvnc_display* display)
{
	LIST_REMOVE(display, link);
	wayvnc_display_detach(display);
	nvnc_display_unref(display->nvnc_display);
	free(display);
}

static void on_output_geometry_change(struct observer* observer, void* data)
{
	struct wayvnc_display* self = wl_container_of(observer, self,
			geometry_change_observer);
	struct output* output = output_from_image_source(self->image_source);
	nvnc_display_set_position(self->nvnc_display, output->x, output->y);

	int width = 0, height = 0;
	image_source_get_transformed_dimensions(self->image_source, &width,
			&height);
	nvnc_display_set_logical_size(self->nvnc_display, width, height);

	nvnc_log(NVNC_LOG_DEBUG, "Output geometry changed: %d, %d", width,
			height);
}

static void on_desktop_output_destroyed(struct observer* observer, void* data)
{
	struct wayvnc_display* self = wl_container_of(observer, self,
			destruction_observer);
	struct wayvnc* wayvnc = self->wayvnc;
	nvnc_remove_display(wayvnc->nvnc, self->nvnc_display);
	wayvnc_display_destroy(self);

	if (!wayland || !wl_list_empty(&wayland->outputs))
		return;

	if (wayvnc->start_detached) {
		nvnc_log(NVNC_LOG_WARNING, "No desktop outputs left. Detaching...");
		wayland_detach(wayvnc);
	} else {
		nvnc_log(NVNC_LOG_ERROR, "No desktop outputs left. Exiting...");
		wayvnc_exit(wayvnc);
	}
}

static bool wayvnc_desktop_display_add(struct wayvnc* self,
		struct image_source* image_source)
{
	struct output* output = output_from_image_source(image_source);
	struct wayvnc_display* display = wayvnc_display_add(self, image_source,
			output->x, output->y);
	if (!display)
		return false;

	int width, height;
	if (image_source_get_transformed_dimensions(display->image_source,
				&width, &height)) {
		nvnc_display_set_logical_size(display->nvnc_display,
				width, height);
	}

	nvnc_trace("setting geometry observer");
	observer_init(&display->geometry_change_observer,
			&display->image_source->observable.geometry_change,
			on_output_geometry_change);

	nvnc_trace("setting destruction observer");
	observer_init(&display->destruction_observer,
			&display->image_source->observable.destroyed,
			on_desktop_output_destroyed);

	return true;
}

static void wayvnc_display_list_init(struct wayvnc* self)
{
	LIST_INIT(&self->wayvnc_displays);

	if (self->image_source && image_source_is_desktop(self->image_source)) {
		struct output* output;
		wl_list_for_each(output, &wayland->outputs, link)
			wayvnc_desktop_display_add(self, &output->image_source);
	} else {
		wayvnc_display_add(self, self->image_source, 0, 0);
	}
}

static void wayvnc_display_list_deinit(struct wayvnc_display_list* list)
{
	while (!LIST_EMPTY(list)) {
		struct wayvnc_display* display = LIST_FIRST(list);
		wayvnc_display_destroy(display);
	}
}

static int init_nvnc(struct wayvnc* self)
{
	self->nvnc = nvnc_new();
	if (!self->nvnc)
		return -1;

	nvnc_set_userdata(self->nvnc, self, NULL);

	nvnc_set_name(self->nvnc, self->desktop_name);

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

	return 0;

auth_failure:
	wayvnc_display_list_deinit(&self->wayvnc_displays);
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

	int rc = image_source_acquire_power_on(self->image_source);
	if (rc == 0) {
		nvnc_log(NVNC_LOG_DEBUG, "Acquired power state management. Waiting for power event to start capturing");
		return 0;
	} else if (rc > 0 && image_source_get_power(self->image_source)
			!= IMAGE_SOURCE_POWER_ON) {
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

static void on_image_source_power_change(struct observer* observer,
		void* arg)
{
	struct wayvnc* self = wl_container_of(observer, self,
			power_change_observer);
	struct image_source* image_source = self->image_source;

	char description[256];
	image_source_describe(image_source, description, sizeof(description));

	enum image_source_power_state state = image_source_get_power(image_source);

	nvnc_trace("%s power state changed to %s", description,
			image_source_power_state_name(state));

	if (self->image_source != image_source || self->nr_clients == 0)
		return;

	switch (state) {
	case IMAGE_SOURCE_POWER_ON:
		wayvnc_start_capture_immediate(self);
		wayvnc_start_cursor_capture(self, true);
		break;
	case IMAGE_SOURCE_POWER_OFF:
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
	output_transform = image_source_get_transform(self->image_source);

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

static void wayvnc_display_send_next_frame(struct wayvnc* self,
		struct wayvnc_display* display, uint64_t now)
{
	struct wv_buffer* buffer = display->next_frame;
	display->next_frame = NULL;

	if (!buffer)
		return;

	struct pixman_region16 damage;
	pixman_region_init(&damage);

	if (screencopy_get_capabilities(self->screencopy)
			& SCREENCOPY_CAP_TRANSFORM) {
		pixman_region_copy(&damage, &buffer->frame_damage);
	} else {
		// TODO: During desktop capture, use correct transform
		apply_output_transform(self, buffer, &damage);
	}

	pixman_region_intersect_rect(&damage, &damage, 0, 0, buffer->width,
			buffer->height);

	nvnc_display_feed_buffer(display->nvnc_display, buffer->nvnc_fb,
			&damage);
	self->n_frames_sent++;

	pixman_region_fini(&damage);

	wayvnc_start_capture(self);

	nvnc_fb_unref(buffer->nvnc_fb);

	self->last_send_time = now;
}

static void wayvnc_send_next_frame(struct wayvnc* self, uint64_t now)
{
	struct wayvnc_display* display;
	LIST_FOREACH(display, &self->wayvnc_displays, link)
		wayvnc_display_send_next_frame(self, display, now);
}

static void wayvnc_handle_rate_limit_timeout(struct aml_timer* timer)
{
	struct wayvnc* self = aml_get_userdata(timer);
	uint64_t now = gettime_us();
	wayvnc_send_next_frame(self, now);
}

static void wayvnc_process_frame(struct wayvnc* self, struct wv_buffer* buffer,
		struct image_source* source)
{
	nvnc_trace("Processing buffer: %p", buffer);

	self->n_frames_captured++;
	self->damage_area_sum += calculate_region_area(&buffer->frame_damage);

	struct wayvnc_display* display =
		wayvnc_display_find_by_source(self, source);
	assert(display);
	if (!display)
		return;

	display->last_frame_info.is_set = true;
	display->last_frame_info.width = buffer->width;
	display->last_frame_info.height = buffer->height;

	if (screencopy_get_capabilities(self->screencopy)
			& SCREENCOPY_CAP_TRANSFORM)
		display->last_frame_info.transform =
			(enum wl_output_transform)nvnc_fb_get_transform(buffer->nvnc_fb);

	bool have_pending_frame = false;
	if (display->next_frame) {
		pixman_region_union(&buffer->frame_damage,
				&buffer->frame_damage,
				&display->next_frame->frame_damage);
		nvnc_fb_unref(display->next_frame->nvnc_fb);
		have_pending_frame = true;
	}
	display->next_frame = buffer;
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
		struct image_source* source, void* userdata)
{
	struct wayvnc* self = userdata;

	switch (result) {
	case SCREENCOPY_FATAL:
		nvnc_log(NVNC_LOG_ERROR, "Failed to capture image. The source probably went away");
		break;
	case SCREENCOPY_FAILED:
		wayvnc_restart_capture(self);
		break;
	case SCREENCOPY_DONE:
		wayvnc_process_frame(self, buffer, source);
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

	double total_area = 0;
	int width, height;
	if (image_source_get_dimensions(self->image_source, &width, &height)) {
		total_area = width * height;
	} else {
		struct wayvnc_display *display;
		LIST_FOREACH(display, &self->wayvnc_displays, link) {
			if (!display->last_frame_info.is_set)
				break;
			width = display->last_frame_info.width;
			height = display->last_frame_info.height;
			total_area += width * height;
		}
	}

	if (total_area == 0)
		return;

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

	if (self->keyboard.virtual_keyboard)
		keyboard_destroy(&self->keyboard);
	self->keyboard.virtual_keyboard = NULL;

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);
	self->pointer.pointer = NULL;

	if (self->data_control.manager)
		data_control_destroy(&self->data_control);
	self->data_control.manager = NULL;
	self->data_control.protocol = DATA_CONTROL_PROTOCOL_NONE;
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

	if (wayland)
		client_init_wayland(self);

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

	if (self->server->exit_on_disconnect && self->server->nr_clients == 0) {
		wayvnc_exit(wayvnc);
	}

	if (wayvnc->ctl) {
		struct ctl_server_client_info info = {};
		compose_client_info(self, &info);

		ctl_server_event_disconnected(wayvnc->ctl, &info,
				wayvnc->nr_clients);
	}

	if (wayvnc->nr_clients == 0 && wayland) {
		nvnc_log(NVNC_LOG_INFO, "Stopping screen capture");
		screencopy_stop(wayvnc->screencopy);
		image_source_release_power_on(wayvnc->image_source);
		stop_performance_ticker(wayvnc);
	}

	if (self->keyboard.virtual_keyboard)
		keyboard_destroy(&self->keyboard);

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

	if (self->nr_clients++ == 0 && wayland) {
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

	struct zwlr_virtual_pointer_manager_v1* pointer_manager =
		wayland->zwlr_virtual_pointer_manager_v1;

	if (!pointer_manager)
		return;

	// TODO
	if (!image_source_is_output(wayvnc->image_source) &&
			!image_source_is_desktop(wayvnc->image_source))
		return;

	int pointer_manager_version =
		zwlr_virtual_pointer_manager_v1_get_version(pointer_manager);

	struct output* output = NULL;
	if (pointer_manager_version >= 2 &&
			image_source_is_output(wayvnc->image_source))
		output = output_from_image_source(wayvnc->image_source);

	self->pointer.vnc = self->server->nvnc;
	self->pointer.image_source = wayvnc->image_source;

	if (self->pointer.pointer)
		pointer_destroy(&self->pointer);

	self->pointer.pointer = output
		? zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
			pointer_manager, self->seat->wl_seat, output->wl_output)
		: zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
			pointer_manager, self->seat->wl_seat);

	if (pointer_init(&self->pointer) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise pointer");
	}

	if (self == wayvnc->cursor_master) {
		// Get seat capability update
		// TODO: Make this asynchronous
		wl_display_roundtrip(wayland->display);
		wl_display_dispatch_pending(wayland->display);

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

	struct seat* seat = seat_find_by_id(&wayland->seats, global_name);
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
	self->transient_seat = ext_transient_seat_manager_v1_create(
			wayland->ext_transient_seat_manager_v1);

	static const struct ext_transient_seat_v1_listener listener = {
		.ready = handle_transient_seat_ready,
		.denied = handle_transient_seat_denied,
	};
	ext_transient_seat_v1_add_listener(self->transient_seat, &listener,
			self);

	// TODO: Make this asynchronous
	wl_display_roundtrip(wayland->display);

	assert(self->seat);
}

static void client_init_seat(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (wayvnc->disable_input)
		return;

	if (wayvnc->selected_seat_name) {
		self->seat = seat_find_by_name(&wayland->seats,
				wayvnc->selected_seat_name);
		assert(self->seat);
	} else if (wayvnc->use_transient_seat) {
		client_init_transient_seat(self);
	} else {
		self->seat = seat_find_unoccupied(&wayland->seats);
		if (!self->seat) {
			self->seat = seat_first(&wayland->seats);
		}
	}

	if (self->seat)
		self->seat->occupancy++;
}

static void client_init_keyboard(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	struct zwp_virtual_keyboard_manager_v1* keyboard_manager =
		wayland->zwp_virtual_keyboard_manager_v1;

	if (!keyboard_manager)
		return;

	self->keyboard.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			keyboard_manager, self->seat->wl_seat);

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
	enum data_control_protocol protocol = DATA_CONTROL_PROTOCOL_NONE;
	void* manager = NULL;

	if (wayland->ext_data_control_manager_v1) {
		protocol = DATA_CONTROL_PROTOCOL_EXT;
		manager = wayland->ext_data_control_manager_v1;
	} else if (wayland->zwlr_data_control_manager_v1) {
		protocol = DATA_CONTROL_PROTOCOL_WLR;
		manager = wayland->zwlr_data_control_manager_v1;
	}

	if (!manager)
		return;

	data_control_init(&self->data_control, protocol, manager,
			wayvnc->nvnc, self->seat->wl_seat);
}

void log_image_source(struct wayvnc* self)
{
	char description[256];
	image_source_describe(self->image_source, description, sizeof(description));

	nvnc_log(NVNC_LOG_INFO, "Capturing %s", description);

	if (!image_source_is_output(self->image_source))
		return;

	// TODO: Maybe remove this?
	struct output* source_output = output_from_image_source(self->image_source);

	struct output* output;
	wl_list_for_each(output, &wayland->outputs, link) {
		bool this_output = (output->id == source_output->id);
		nvnc_log(NVNC_LOG_INFO, "%s %s %dx%d+%dx%d Power:%s",
				this_output ? ">>" : "--",
				output->description,
				output->width, output->height,
				output->x, output->y,
				image_source_power_state_name(output->power));
	}
}

static void wayvnc_process_cursor(struct wayvnc* self, struct wv_buffer* buffer,
		struct image_source* source)
{
	nvnc_log(NVNC_LOG_DEBUG, "Got new cursor");
	bool is_damaged = pixman_region_not_empty(&buffer->frame_damage);
	nvnc_set_cursor(self->nvnc, buffer->nvnc_fb, buffer->width,
			buffer->height, buffer->x_hotspot, buffer->y_hotspot,
			is_damaged);
	wayvnc_start_cursor_capture(self, false);
}

static void on_cursor_capture_done(enum screencopy_result result,
		struct wv_buffer* buffer, struct image_source* source,
		void* userdata)
{
	struct wayvnc* self = userdata;

	switch (result) {
	case SCREENCOPY_FATAL:
		nvnc_log(NVNC_LOG_ERROR, "Failed to capture cursor. The source probably went away");
		break;
	case SCREENCOPY_FAILED:
		wayvnc_start_cursor_capture(self, true);
		break;
	case SCREENCOPY_DONE:
		wayvnc_process_cursor(self, buffer, source);
		break;
	}
}

static enum nvnc_fb_type buffer_type_to_nvnc_fb_type(enum wv_buffer_type type)
{
	switch (type) {
	case WV_BUFFER_SHM:
		return NVNC_FB_SIMPLE;
#ifdef ENABLE_SCREENCOPY_DMABUF
	case WV_BUFFER_DMABUF:
		return NVNC_FB_GBM_BO;
#endif
	case WV_BUFFER_UNSPEC:;
	}
	return NVNC_FB_UNSPEC;
}

static double rate_output_format(const void* userdata,
		enum wv_buffer_type type, uint32_t format, uint64_t modifier)
{
	const struct wayvnc* self = userdata;

	enum nvnc_fb_type fb_type = buffer_type_to_nvnc_fb_type(type);
	assert(fb_type != NVNC_FB_UNSPEC);

	return nvnc_rate_pixel_format(self->nvnc, fb_type, format,
			modifier);
}

static double rate_cursor_format(const void* userdata,
		enum wv_buffer_type type, uint32_t format, uint64_t modifier)
{
	const struct wayvnc* self = userdata;

	enum nvnc_fb_type fb_type = buffer_type_to_nvnc_fb_type(type);
	assert(fb_type != NVNC_FB_UNSPEC);

	return nvnc_rate_cursor_pixel_format(self->nvnc, fb_type, format,
			modifier);
}

static bool configure_cursor_sc(struct wayvnc* self,
		struct wayvnc_client* client)
{
	nvnc_log(NVNC_LOG_DEBUG, "Configuring cursor capturing");

	screencopy_stop(self->cursor_sc);
	screencopy_destroy(self->cursor_sc);
	self->cursor_sc = NULL;

	struct seat* seat = client->seat;
	assert(seat);

	if (!(seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		nvnc_log(NVNC_LOG_DEBUG, "Client's seat has no pointer capability");
		return false;
	}

	self->cursor_sc = screencopy_create_cursor(self->image_source,
			seat->wl_seat);
	if (!self->cursor_sc) {
		nvnc_log(NVNC_LOG_DEBUG, "Failed to capture cursor");
		return false;
	}

	self->cursor_sc->on_done = on_cursor_capture_done;
	self->cursor_sc->rate_format = rate_cursor_format;
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

	self->screencopy = screencopy_create(self->image_source,
			self->overlay_cursor);
	if (!self->screencopy) {
		nvnc_log(NVNC_LOG_ERROR, "screencopy is not supported by compositor");
		return false;
	}

	self->screencopy->on_done = on_capture_done;
	self->screencopy->rate_format = rate_output_format;
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

static void on_toplevel_closed(struct toplevel* toplevel)
{
	struct wayvnc* self = toplevel->userdata;
	nvnc_log(NVNC_LOG_ERROR, "Toplevel was closed. Exiting...");
	wayvnc_exit(self);
}

static void on_image_source_destroyed(struct observer* observer, void* arg)
{
	struct wayvnc* self = wl_container_of(observer, self,
			destruction_observer);
	self->image_source = NULL;
}

void set_image_source(struct wayvnc* self, struct image_source* image_source)
{
	if (self->image_source) {
		observer_deinit(&self->power_change_observer);
		observer_deinit(&self->destruction_observer);
	}
	self->image_source = image_source;
	observer_init(&self->power_change_observer,
			&image_source->observable.power_change,
			on_image_source_power_change);

	observer_init(&self->destruction_observer,
			&image_source->observable.destroyed,
			on_image_source_destroyed);

	if (image_source_is_toplevel(image_source)) {
		struct toplevel* toplevel =
			toplevel_from_image_source(image_source);
		toplevel->on_closed = on_toplevel_closed;
		toplevel->userdata = self;
	}

	if (self->ctl && image_source_is_output(image_source)) {
		ctl_server_event_capture_changed(self->ctl,
				output_from_image_source(image_source)->name);
	}

	log_image_source(self);
}

void switch_to_output(struct wayvnc* self, struct output* output)
{
	assert(image_source_is_output(self->image_source));
	if (output_from_image_source(self->image_source) == output) {
		nvnc_log(NVNC_LOG_INFO, "Already selected output %s",
				output->name);
		return;
	}
	screencopy_stop(self->screencopy);
	output_release_power_on(output);
	set_image_source(self, &output->image_source);
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
	struct output* next = output_cycle(&wayland->outputs,
			output_from_image_source(self->image_source),
			OUTPUT_CYCLE_FORWARD);
	switch_to_output(self, next);
}

void switch_to_prev_output(struct wayvnc* self)
{
	nvnc_log(NVNC_LOG_INFO, "Rotating to previous output");
	struct output* prev = output_cycle(&wayland->outputs,
			output_from_image_source(self->image_source),
			OUTPUT_CYCLE_REVERSE);
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

static struct cmd_response* on_attach(struct ctl* ctl, const char* display,
		enum image_source_type image_source_type,
		const char* image_source_name)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	assert(self);

	memset(intercepted_error, 0, sizeof(intercepted_error));
	nvnc_set_log_fn_thread_local(intercept_cmd_error);

	bool ok = wayland_attach(self, display, image_source_type,
			image_source_name);

	wayvnc_display_list_deinit(&self->wayvnc_displays);
	wayvnc_display_list_init(self);
	blank_screen(self);

	struct nvnc_client* nvnc_client;
	for (nvnc_client = nvnc_client_first(self->nvnc); nvnc_client;
			nvnc_client = nvnc_client_next(nvnc_client)) {
		struct wayvnc_client* client =
			nvnc_get_userdata(nvnc_client);
		client_init_wayland(client);
	}

	nvnc_log(NVNC_LOG_INFO, "Attached to %s", display);

	if (self->nr_clients > 0) {
		handle_first_client(self);
	}

	nvnc_set_log_fn_thread_local(NULL);

	return ok ? cmd_ok() : cmd_failed("%s", intercepted_error);
}

// TODO: Also add arguments for seat
static bool wayland_attach(struct wayvnc* self, const char* display,
		enum image_source_type image_source_type,
		const char* image_source_name)
{
	if (wayland) {
		wayland_detach(self);
	}

	if (image_source_type) {
		self->image_source_type = image_source_type;
		self->image_source_name[0] = '\0';
		if (image_source_name) {
			strlcpy(self->image_source_name, image_source_name,
					sizeof(self->image_source_name));
		}
	}

	nvnc_log(NVNC_LOG_DEBUG, "Attaching to %s", display);

	if (init_wayland(self, display) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise wayland");
		goto failure;
	}

	if (!wayland->zwlr_screencopy_manager_v1 &&
			!wayland->ext_image_copy_capture_manager_v1) {
		nvnc_log(NVNC_LOG_ERROR, "Attached display does not implement screencapturing");
		goto failure;
	}

	switch (self->image_source_type) {
	case IMAGE_SOURCE_TYPE_OUTPUT:;
		struct output* out;
		out = output_find_by_name(&wayland->outputs,
				self->image_source_name);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No such output");
			goto failure;
		}
		set_image_source(self, &out->image_source);
		break;
	case IMAGE_SOURCE_TYPE_DESKTOP:;
		struct desktop* desktop = desktop_new(&wayland->outputs);
		if (!desktop) {
			nvnc_log(NVNC_LOG_ERROR, "Failed to set up desktop capture");
			goto failure;
		}
		set_image_source(self, &desktop->image_source);
		break;
	case IMAGE_SOURCE_TYPE_TOPLEVEL:;
		struct toplevel* toplevel;
		toplevel = toplevel_find_by_identifier(&wayland->toplevels,
				self->image_source_name);
		if (!toplevel) {
			nvnc_log(NVNC_LOG_ERROR, "No such toplevel");
			goto failure;
		}
		set_image_source(self, &toplevel->image_source);
		break;
	case IMAGE_SOURCE_TYPE_UNSPEC:;
		out = output_first(&wayland->outputs);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No output found");
			goto failure;
		}
		set_image_source(self, &out->image_source);
	}

	if (self->selected_seat_name) {
		struct seat* seat = seat_find_by_name(&wayland->seats,
				self->selected_seat_name);
		if (!seat) {
			nvnc_log(NVNC_LOG_ERROR, "No such seat: %s",
					self->selected_seat_name);
			goto failure;
		}
	}

	if (!configure_screencopy(self)) {
		goto failure;
	}

	return true;

failure:
	wayland_detach(self);
	return false;
}

static struct cmd_response* on_detach(struct ctl* ctl)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	assert(self);

	if (!wayland) {
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
	bool use_desktop_capture = false;
	bool use_external_fd = false;
	bool use_unix_socket = false;
	bool use_websocket = false;
	bool start_detached = false;

	const char* output_name = NULL;
	const char* seat_name = NULL;
	const char* socket_path = NULL;
	const char* keyboard_options = NULL;
	const char* toplevel_id;

	bool overlay_cursor = false;
	bool show_performance = false;
	int max_rate = 30;
	bool disable_input = false;
	bool use_transient_seat = false;
	bool exit_on_disconnect = false;

	const char* log_level_name = NULL;
	const char* log_filter = NULL;
	bool is_verbose = false;
	int log_level = NVNC_LOG_WARNING;

	static const struct wv_option opts[] = {
		{ .positional = "address",
		  .help = "An address to listen on.",
		  .default_ = "localhost",
		  .is_repeating = true },
		{ 'a', "desktop", NULL,
		  "Capture all outputs." },
		{ 'C', "config", "<path>",
		  "Select a config file." },
		{ 'd', "disable-input", NULL,
		  "Disable all remote input." },
		{ 'D', "detached", NULL,
		  "Start detached from a compositor." },
		{ 'e', "exit-on-disconnect", NULL,
		  "Exit when last client disconnects." },
		{ 'f', "max-fps", "<fps>",
		  "Set rate limit.",
		  .default_ = "30" },
		{ 'F', "log-filter", "<string>",
		  "Set log filter." },
		{ 'g', "gpu", NULL,
		  "Enable features that need GPU." },
		{ 'h', "help", NULL,
		  "Get help (this text)." },
		{ 'k', "keyboard", "<layout>[-<variant>]",
		  "Select keyboard layout with an optional variant." },
		{ 'L', "log-level", "<level>",
		  "Set log level. The levels are: error, warning, info, debug trace and quiet.",
		  .default_ = "warning" },
		{ 'n', "name", "<name>",
		  "Set the desktop name.",
		  .default_ = "WayVNC" },
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
		{ 'T', "toplevel", "<identifier>",
		  "Capture a toplevel" },
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

	use_desktop_capture = option_parser_get_value(&option_parser, "desktop");
	cfg_file = option_parser_get_value(&option_parser, "config");
	enable_gpu_features = !!option_parser_get_value(&option_parser, "gpu");
	self.desktop_name = option_parser_get_value(&option_parser, "name");
	output_name = option_parser_get_value(&option_parser, "output");
	seat_name = option_parser_get_value(&option_parser, "seat");
	socket_path = option_parser_get_value(&option_parser, "socket");
	overlay_cursor = !!option_parser_get_value(&option_parser, "render-cursor");
	show_performance = !!option_parser_get_value(&option_parser,
			"show-performance");
	exit_on_disconnect = !!option_parser_get_value(&option_parser, "exit-on-disconnect");
	use_unix_socket = !!option_parser_get_value(&option_parser, "unix-socket");
	use_websocket = !!option_parser_get_value(&option_parser, "websocket");
	use_external_fd = !!option_parser_get_value(&option_parser,
			"external-listener-fd");
	disable_input = !!option_parser_get_value(&option_parser, "disable-input");
	is_verbose = option_parser_get_value(&option_parser, "verbose");
	log_level_name = option_parser_get_value(&option_parser, "log-level");
	log_filter = option_parser_get_value(&option_parser, "log-filter");
	max_rate = atoi(option_parser_get_value(&option_parser, "max-fps"));
	use_transient_seat = !!option_parser_get_value(&option_parser,
				"transient-seat");
	toplevel_id = option_parser_get_value(&option_parser, "toplevel");
	start_detached = !!option_parser_get_value(&option_parser, "detached");
	self.enable_resizing = !option_parser_get_value(&option_parser,
			"disable-resizing");

	self.start_detached = start_detached;
	self.exit_on_disconnect = exit_on_disconnect;
	self.overlay_cursor = overlay_cursor;
	self.max_rate = max_rate;
	self.enable_gpu_features = enable_gpu_features;
	self.use_toplevel = !!toplevel_id;
	self.selected_seat_name = seat_name;

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

	if (log_filter)
		nvnc_set_log_filter(log_filter);

	if (seat_name && disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "seat and disable-input are conflicting options");
		return 1;
	}

	int n_address_modifiers = use_unix_socket + use_external_fd;
	if (n_address_modifiers > 1) {
		nvnc_log(NVNC_LOG_ERROR, "Only one of unix-socket or the external-listener-fd options may be set");
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

	if (toplevel_id && output_name) {
		nvnc_log(NVNC_LOG_ERROR, "toplevel and output are conflicting options");
		return 1;
	}

	if (toplevel_id && start_detached) {
		nvnc_log(NVNC_LOG_ERROR, "toplevel and start-detached are conflicting options");
		return 1;
	}

	if (use_desktop_capture && output_name) {
		nvnc_log(NVNC_LOG_ERROR, "desktop and output are conflicting options");
		return 1;
	}

	if (use_desktop_capture && toplevel_id) {
		nvnc_log(NVNC_LOG_ERROR, "desktop and toplevel are conflicting options");
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

	if (output_name) {
		self.image_source_type = IMAGE_SOURCE_TYPE_OUTPUT;
		strlcpy(self.image_source_name, output_name,
				sizeof(self.image_source_name));
	} else if (use_desktop_capture) {
		self.image_source_type = IMAGE_SOURCE_TYPE_DESKTOP;
	} else if (toplevel_id) {
		self.image_source_type = IMAGE_SOURCE_TYPE_OUTPUT;
		strlcpy(self.image_source_name, toplevel_id,
				sizeof(self.image_source_name));
	} else {
		self.image_source_type = IMAGE_SOURCE_TYPE_UNSPEC;
	}

	if (!start_detached)
		if (!wayland_attach(&self, NULL, IMAGE_SOURCE_TYPE_UNSPEC,
					NULL))
			goto wayland_failure;

	enum socket_type default_socket_type = SOCKET_TYPE_TCP;
	if (use_unix_socket)
		default_socket_type = SOCKET_TYPE_UNIX;
	else if (use_external_fd)
		default_socket_type = SOCKET_TYPE_FROM_FD;
	enum nvnc_stream_type default_stream_type = NVNC_STREAM_NORMAL;
	if (use_websocket)
		default_stream_type = NVNC_STREAM_WEBSOCKET;

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
		.on_set_desktop_name = on_set_desktop_name,
		.get_output_list = get_output_list,
		.on_disconnect_client = on_disconnect_client,
		.on_wayvnc_exit = on_wayvnc_exit,
	};
	self.ctl = ctl_server_new(socket_path, &ctl_actions);
	if (!self.ctl)
		goto ctl_server_failure;

	if (init_nvnc(&self) < 0)
		goto nvnc_failure;

	wayvnc_display_list_init(&self);
	blank_screen(&self);

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
				default_socket_type, default_stream_type) < 0)
			goto nvnc_failure;
	} else if (!option_parser_get_value_with_offset(&option_parser,
				"address", 0)) {
		if (apply_addresses_from_config(&self, default_socket_type,
					default_stream_type) < 0)
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
					default_socket_type,
					default_stream_type) < 0)
				goto nvnc_failure;
		}
	}

	if (wayland)
		wl_display_dispatch_pending(wayland->display);

	while (!self.do_exit) {
		if (wayland)
			wl_display_flush(wayland->display);

		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	nvnc_log(NVNC_LOG_INFO, "Exiting...");

	ctl_server_destroy(self.ctl);
	self.ctl = NULL;

	wayvnc_display_list_deinit(&self.wayvnc_displays);
	nvnc_del(self.nvnc);
	self.nvnc = NULL;
	wayland_destroy(wayland);

	aml_stop(aml, self.rate_limiter);
	aml_unref(self.rate_limiter);
	aml_unref(aml);

	cfg_destroy(&self.cfg);

	return 0;

nvnc_failure:
	ctl_server_destroy(self.ctl);
	self.ctl = NULL;
	wayvnc_display_list_deinit(&self.wayvnc_displays);
	nvnc_del(self.nvnc);
	self.nvnc = NULL;
ctl_server_failure:
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
