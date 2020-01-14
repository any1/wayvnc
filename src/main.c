/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <inttypes.h>
#include <neatvnc.h>
#include <uv.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <pixman.h>

#include "wlr-export-dmabuf-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "virtual-keyboard-unstable-v1.h"
#include "render.h"
#include "dmabuf.h"
#include "screencopy.h"
#include "strlcpy.h"
#include "logging.h"
#include "output.h"
#include "pointer.h"
#include "keyboard.h"
#include "seat.h"

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

enum frame_capture_backend_type {
	FRAME_CAPTURE_BACKEND_NONE = 0,
	FRAME_CAPTURE_BACKEND_SCREENCOPY,
	FRAME_CAPTURE_BACKEND_DMABUF,
};

struct wayvnc {
	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_list outputs;
	struct wl_list seats;

	struct zwp_virtual_keyboard_manager_v1* keyboard_manager;

	struct renderer renderer;
	const struct output* selected_output;
	const struct seat* selected_seat;

	struct dmabuf_capture dmabuf_backend;
	struct screencopy screencopy_backend;
	struct frame_capture* capture_backend;
	struct pointer pointer_backend;
	struct keyboard keyboard_backend;

	uv_poll_t wayland_poller;
	uv_prepare_t flusher;
	uv_signal_t signal_handler;
	uv_timer_t performance_timer;

	struct nvnc* nvnc;

	struct nvnc_fb* current_fb;
	struct nvnc_fb* last_fb;

	uint32_t n_frames;

	const char* kb_layout;
};

void on_capture_done(struct frame_capture* capture);

static enum frame_capture_backend_type
frame_capture_backend_from_string(const char* str)
{
	if (strcmp(str, "screencopy") == 0)
		return FRAME_CAPTURE_BACKEND_SCREENCOPY;

	if (strcmp(str, "dmabuf") == 0)
		return FRAME_CAPTURE_BACKEND_DMABUF;

	return FRAME_CAPTURE_BACKEND_NONE;
}

static void registry_add(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface,
			 uint32_t version)
{
	struct wayvnc* self = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output* wl_output =
			wl_registry_bind(registry, id, &wl_output_interface,
					 version);
		if (!wl_output)
			return;

		struct output* output = output_new(wl_output, id);
		if (!output)
			return;

		wl_list_insert(&self->outputs, &output->link);
		return;
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		self->screencopy_backend.wl_shm
			= wl_registry_bind(registry, id, &wl_shm_interface, 1);
		return;
	}

	if (strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
		self->dmabuf_backend.manager =
			wl_registry_bind(registry, id,
					 &zwlr_export_dmabuf_manager_v1_interface,
					 1);
		return;
	}

	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		self->screencopy_backend.manager =
			wl_registry_bind(registry, id,
					 &zwlr_screencopy_manager_v1_interface,
					 2);
		return;
	}

	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		self->pointer_backend.manager =
			wl_registry_bind(registry, id,
					 &zwlr_virtual_pointer_manager_v1_interface,
					 1);
		return;
	}

	if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat* wl_seat =
			wl_registry_bind(registry, id, &wl_seat_interface, 7);
		if (!wl_seat)
			return;

		struct seat* seat = seat_new(wl_seat);
		if (!seat) {
			wl_seat_destroy(wl_seat);
			return;
		}

		wl_list_insert(&self->seats, &seat->link);
		return;
	}

	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		self->keyboard_manager =
			wl_registry_bind(registry, id,
			                 &zwp_virtual_keyboard_manager_v1_interface,
			                 1);
		return;
	}
}

static void registry_remove(void* data, struct wl_registry* registry,
			    uint32_t id)
{
	struct wayvnc* self = data;

	struct output* out = output_find_by_id(&self->outputs, id);
	if (out) {
		wl_list_remove(&out->link);
		output_destroy(out);

		/* TODO: If this is the selected output, exit */
	}
}

void wayvnc_destroy(struct wayvnc* self)
{
	output_list_destroy(&self->outputs);
	seat_list_destroy(&self->seats);

	wl_shm_destroy(self->screencopy_backend.wl_shm);

	zwp_virtual_keyboard_v1_destroy(self->keyboard_backend.virtual_keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(self->keyboard_manager);
	keyboard_destroy(&self->keyboard_backend);

	zwlr_virtual_pointer_manager_v1_destroy(self->pointer_backend.manager);
	pointer_destroy(&self->pointer_backend);

	if (self->screencopy_backend.manager)
		zwlr_screencopy_manager_v1_destroy(self->screencopy_backend.manager);

	if (self->dmabuf_backend.manager)
		zwlr_export_dmabuf_manager_v1_destroy(self->dmabuf_backend.manager);

	wl_display_disconnect(self->display);
}

static int init_wayland(struct wayvnc* self)
{
	static const struct wl_registry_listener registry_listener = {
		.global = registry_add,
		.global_remove = registry_remove,
	};

	self->display = wl_display_connect(NULL);
	if (!self->display)
		return -1;

	wl_list_init(&self->outputs);
	wl_list_init(&self->seats);

	self->registry = wl_display_get_registry(self->display);
	if (!self->registry)
		goto failure;

	wl_registry_add_listener(self->registry, &registry_listener, self);

	wl_display_roundtrip(self->display);
	wl_display_dispatch(self->display);

	if (!self->dmabuf_backend.manager && !self->screencopy_backend.manager) {
		log_error("Compositor supports neither screencopy nor export-dmabuf! Exiting.\n");
		goto failure;
	}

	self->dmabuf_backend.fc.on_done = on_capture_done;
	self->dmabuf_backend.fc.userdata = self;

	self->screencopy_backend.frame_capture.on_done = on_capture_done;
	self->screencopy_backend.frame_capture.userdata = self;

	return 0;

failure:
	wl_display_disconnect(self->display);
	return -1;
}

void on_wayland_event(uv_poll_t* handle, int status, int event)
{
	struct wayvnc* self = wl_container_of(handle, self, wayland_poller);
	wl_display_dispatch(self->display);
}

void prepare_for_poll(uv_prepare_t* handle)
{
	struct wayvnc* self = wl_container_of(handle, self, flusher);
	wl_display_flush(self->display);
}

void on_uv_walk(uv_handle_t* handle, void* arg)
{
	uv_unref(handle);
}

void wayvnc_exit(void)
{
	uv_walk(uv_default_loop(), on_uv_walk, NULL);
}

void on_signal(uv_signal_t* handle, int signo)
{
	wayvnc_exit();
}

void on_performance_tick(uv_timer_t* handle)
{
	struct wayvnc* self = wl_container_of(handle, self, performance_timer);

	printf("%"PRIu32" FPS\n", self->n_frames);
	self->n_frames = 0;
}

int init_main_loop(struct wayvnc* self)
{
	uv_loop_t* loop = uv_default_loop();

	uv_poll_init(loop, &self->wayland_poller,
		     wl_display_get_fd(self->display));
	uv_poll_start(&self->wayland_poller, UV_READABLE, on_wayland_event);

	uv_prepare_init(loop, &self->flusher);
	uv_prepare_start(&self->flusher, prepare_for_poll);

	uv_signal_init(loop, &self->signal_handler);
	uv_signal_start(&self->signal_handler, on_signal, SIGINT);

	uv_timer_init(loop, &self->performance_timer);
	uv_timer_start(&self->performance_timer, on_performance_tick, 1000,
		       1000);

	return 0;
}

uint32_t fourcc_from_gl_format(uint32_t format)
{
	switch (format) {
	case GL_BGRA_EXT: return DRM_FORMAT_XRGB8888;
	case GL_RGBA: return DRM_FORMAT_XBGR8888;
	}

	return DRM_FORMAT_INVALID;
}

static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
			     enum nvnc_button_mask button_mask)
{
	// TODO: Have a seat per client

	struct nvnc* nvnc = nvnc_get_server(client);
	struct wayvnc* wayvnc = nvnc_get_userdata(nvnc);

	pointer_set(&wayvnc->pointer_backend, x, y, button_mask);
}

static void on_key_event(struct nvnc_client* client, uint32_t symbol,
                         bool is_pressed)
{
	struct nvnc* nvnc = nvnc_get_server(client);
	struct wayvnc* wayvnc = nvnc_get_userdata(nvnc);

	keyboard_feed(&wayvnc->keyboard_backend, symbol, is_pressed);
}

int init_nvnc(struct wayvnc* self, const char* addr, uint16_t port)
{
	self->nvnc = nvnc_open(addr, port);
	if (!self->nvnc)
		return -1;

	nvnc_set_userdata(self->nvnc, self);

	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);
	if (format == DRM_FORMAT_INVALID)
		return -1;

	nvnc_set_name(self->nvnc, "WayVNC");
	nvnc_set_dimensions(self->nvnc,
			    self->selected_output->width,
			    self->selected_output->height,
			    format);

	if (self->pointer_backend.manager)
		nvnc_set_pointer_fn(self->nvnc, on_pointer_event);

	if (self->keyboard_backend.virtual_keyboard)
		nvnc_set_key_fn(self->nvnc, on_key_event);

	return 0;
}

int wayvnc_start_capture(struct wayvnc* self)
{
	return frame_capture_start(self->capture_backend);
}

void on_damage_check_done(struct pixman_region16* damage, void* userdata)
{
	struct wayvnc* self = userdata;

	if (pixman_region_not_empty(damage))
		nvnc_feed_frame(self->nvnc, self->current_fb, damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit();
	}
}

void wayvnc_update_vnc(struct wayvnc* self, struct nvnc_fb* fb)
{
	uint32_t width = nvnc_fb_get_width(fb);
	uint32_t height = nvnc_fb_get_height(fb);

	if (self->last_fb)
		nvnc_fb_unref(self->last_fb);

	self->last_fb = self->current_fb;
	self->current_fb = fb;

	if (self->last_fb) {
		uint32_t hint_x = self->capture_backend->damage_hint.x;
		uint32_t hint_y = self->capture_backend->damage_hint.y;
		uint32_t hint_width = self->capture_backend->damage_hint.width;
		uint32_t hint_height = self->capture_backend->damage_hint.height;

		nvnc_check_damage(self->current_fb, self->last_fb, hint_x,
				  hint_y, hint_width, hint_height,
				  on_damage_check_done, self);
		return;
	}

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, width, height);
	nvnc_feed_frame(self->nvnc, self->current_fb, &damage);
	pixman_region_fini(&damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit();
	}
}

void wayvnc_process_frame(struct wayvnc* self)
{
	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);
	struct dmabuf_frame* frame = &self->dmabuf_backend.frame;

	struct nvnc_fb* fb = nvnc_fb_new(frame->width, frame->height, format);
	void* addr = nvnc_fb_get_addr(fb);

	render_dmabuf_frame(&self->renderer, frame);
	render_copy_pixels(&self->renderer, addr, 0, frame->height);

	wayvnc_update_vnc(self, fb);
}

void wayvnc_process_screen(struct wayvnc* self)
{
	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);

	void* pixels = self->screencopy_backend.pixels;

	uint32_t width = self->capture_backend->frame_info.width;
	uint32_t height = self->capture_backend->frame_info.height;
	uint32_t stride = self->capture_backend->frame_info.stride;

	struct nvnc_fb* fb = nvnc_fb_new(width, height, format);
	void* addr = nvnc_fb_get_addr(fb);

	render_framebuffer(&self->renderer, pixels, format, width, height,
			   stride);
	render_copy_pixels(&self->renderer, addr, 0, height);

	wayvnc_update_vnc(self, fb);
}

void on_capture_done(struct frame_capture* capture)
{
	struct wayvnc* self = capture->userdata;

	switch (capture->status) {
	case CAPTURE_STOPPED:
		break;
	case CAPTURE_IN_PROGRESS:
		break;
	case CAPTURE_FATAL:
		log_error("Fatal error while capturing. Exiting...\n");
		wayvnc_exit();
		break;
	case CAPTURE_FAILED:
		if (wayvnc_start_capture(self) < 0) {
			log_error("Failed to start capture. Exiting...\n");
			wayvnc_exit();
		}
		break;
	case CAPTURE_DONE:
		self->n_frames++;
		if (self->capture_backend == (struct frame_capture*)&self->screencopy_backend)
			wayvnc_process_screen(self);
		else
			wayvnc_process_frame(self);
		break;
	}
}

int wayvnc_usage(FILE* stream, int rc)
{
	static const char* usage =
"Usage: wayvnc [options] [address [port]]\n"
"\n"
"    -c,--frame-capturing=screencopy|dmabuf    Select frame capturing backend.\n"
"    -o,--output=<id>                          Select output to capture.\n"
"    -k,--keyboard=<layout>                    Select keyboard layout.\n"
"    -s,--seat=<name>                          Select seat by name.\n"
"    -h,--help                                 Get help (this text).\n"
"\n";

	fprintf(stream, "%s", usage);

	return rc;
}

int main(int argc, char* argv[])
{
	struct wayvnc self = { 0 };

	const char* address = DEFAULT_ADDRESS;
	int port = DEFAULT_PORT;

	int output_id = -1;
	enum frame_capture_backend_type fcbackend = FRAME_CAPTURE_BACKEND_NONE;
	const char* seat_name = NULL;

	static const char* shortopts = "c:o:k:s:h";

	static const struct option longopts[] = {
		{ "frame-capturing", required_argument, NULL, 'c' },
		{ "output", required_argument, NULL, 'o' },
		{ "keyboard", required_argument, NULL, 'k' },
		{ "seat", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'c':
			fcbackend = frame_capture_backend_from_string(optarg);
			if (fcbackend == FRAME_CAPTURE_BACKEND_NONE) {
				fprintf(stderr, "Invalid backend: %s\n\n",
					optarg);

				return wayvnc_usage(stderr, 1);
			}
			break;
		case 'o':
			output_id = atoi(optarg);
			break;
		case 'k':
			self.kb_layout = optarg;
			break;
		case 's':
			seat_name = optarg;
			break;
		case 'h':
			return wayvnc_usage(stdout, 0);
		default:
			return wayvnc_usage(stderr, 1);
		}
	}

	int n_args = argc - optind;

	if (n_args >= 1)
		address = argv[optind];

	if (n_args >= 2)
		port = atoi(argv[optind + 1]);

	if (init_wayland(&self) < 0) {
		log_error("Failed to initialise wayland\n");
		return 1;
	}

	printf("Outputs:\n");

	struct output* out;
	wl_list_for_each(out, &self.outputs, link)
		printf("%"PRIu32": Make: %s. Model: %s\n", out->id, out->make,
		       out->model);

	if (output_id >= 0) {
		out = output_find_by_id(&self.outputs, output_id);
		if (!out) {
			log_error("No such output\n");
			goto failure;
		}
	} else {
		out = output_first(&self.outputs);
		if (!out) {
			log_error("No output found\n");
			goto failure;
		}
	}

	struct seat* seat;
	if (seat_name) {
		seat = seat_find_by_name(&self.seats, seat_name);
		if (!seat) {
			log_error("No such seat\n");
			goto failure;
		}
	} else {
		seat = seat_first(&self.seats);
		if (!seat) {
			log_error("No seat found\n");
			goto failure;
		}
	}

	self.selected_output = out;
	self.selected_seat = seat;
	self.dmabuf_backend.fc.wl_output = out->wl_output;
	self.screencopy_backend.frame_capture.wl_output = out->wl_output;

	if (self.pointer_backend.manager) {
		self.pointer_backend.width = out->width;
		self.pointer_backend.height = out->height;
	}

	assert(self.keyboard_manager);

	self.keyboard_backend.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			self.keyboard_manager, self.selected_seat->wl_seat);

	keyboard_init(&self.keyboard_backend, self.kb_layout);

	if (self.pointer_backend.manager) {
		self.pointer_backend.vnc = self.nvnc;
		pointer_init(&self.pointer_backend, self.selected_seat->wl_seat);
	} else {
		log_error("Compositor does not support %s.\n",
			  zwlr_virtual_pointer_manager_v1_interface.name);
		goto failure;
	}

	if (renderer_init(&self.renderer, self.selected_output->width,
			  self.selected_output->height) < 0) {
		log_error("Failed to initialise renderer\n");
		goto failure;
	}

	if (init_main_loop(&self) < 0)
		goto main_loop_failure;

	if (init_nvnc(&self, address, port) < 0)
		goto nvnc_failure;

	if (self.screencopy_backend.manager)
		screencopy_init(&self.screencopy_backend);

	if (self.dmabuf_backend.manager)
		dmabuf_capture_init(&self.dmabuf_backend);

	switch (fcbackend) {
	case FRAME_CAPTURE_BACKEND_SCREENCOPY:
		if (!self.screencopy_backend.manager) {
			log_error("screencopy is not supported by compositor\n");
			goto capture_failure;
		}

		self.capture_backend = &self.screencopy_backend.frame_capture;
		break;
	case FRAME_CAPTURE_BACKEND_DMABUF:
		if (!self.screencopy_backend.manager) {
			log_error("export-dmabuf is not supported by compositor\n");
			goto capture_failure;
		}

		self.capture_backend = &self.dmabuf_backend.fc;
		break;
	case FRAME_CAPTURE_BACKEND_NONE:
		if (self.screencopy_backend.manager)
			self.capture_backend = &self.screencopy_backend.frame_capture;
		else if (self.dmabuf_backend.manager)
			self.capture_backend = &self.dmabuf_backend.fc;
		else
			goto capture_failure;
		break;
	}

	if (wayvnc_start_capture(&self) < 0)
		goto capture_failure;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	uv_loop_close(uv_default_loop());

	frame_capture_stop(self.capture_backend);

	if (self.current_fb) nvnc_fb_unref(self.current_fb);
	if (self.last_fb) nvnc_fb_unref(self.last_fb);

	nvnc_close(self.nvnc);
	renderer_destroy(&self.renderer);
	screencopy_destroy(&self.screencopy_backend);
	wayvnc_destroy(&self);

	return 0;

capture_failure:
	nvnc_close(self.nvnc);
nvnc_failure:
main_loop_failure:
	renderer_destroy(&self.renderer);
failure:
	wayvnc_destroy(&self);
	return 1;
}
