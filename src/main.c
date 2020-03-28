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
#include <errno.h>
#include <neatvnc.h>
#include <aml.h>
#include <signal.h>
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
#include "xdg-output-unstable-v1.h"
#include "render.h"
#include "dmabuf.h"
#include "screencopy.h"
#include "strlcpy.h"
#include "logging.h"
#include "output.h"
#include "pointer.h"
#include "keyboard.h"
#include "seat.h"
#include "cfg.h"
#include "damage.h"

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

enum frame_capture_backend_type {
	FRAME_CAPTURE_BACKEND_NONE = 0,
	FRAME_CAPTURE_BACKEND_SCREENCOPY,
	FRAME_CAPTURE_BACKEND_DMABUF,
};

struct wayvnc {
	bool do_exit;

	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_list outputs;
	struct wl_list seats;
	struct cfg cfg;

	struct zxdg_output_manager_v1* xdg_output_manager;
	struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
	struct zwlr_virtual_pointer_manager_v1* pointer_manager;

	int pointer_manager_version;

	struct renderer renderer;
	const struct output* selected_output;
	const struct seat* selected_seat;

	struct dmabuf_capture dmabuf_backend;
	struct screencopy screencopy_backend;
	struct frame_capture* capture_backend;
	struct pointer pointer_backend;
	struct keyboard keyboard_backend;

	struct aml_handler* wayland_handler;
	struct aml_signal* signal_handler;

	struct nvnc* nvnc;

	struct nvnc_fb* current_fb;
	struct nvnc_fb* last_fb;

	const char* kb_layout;
};

void wayvnc_exit(struct wayvnc* self);
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

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		self->xdg_output_manager =
			wl_registry_bind(registry, id,
			                 &zxdg_output_manager_v1_interface, 3);
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
		self->pointer_manager =
			wl_registry_bind(registry, id,
					 &zwlr_virtual_pointer_manager_v1_interface,
					 version);
		self->pointer_manager_version = version;
		return;
	}

	if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat* wl_seat =
			wl_registry_bind(registry, id, &wl_seat_interface, 7);
		if (!wl_seat)
			return;

		struct seat* seat = seat_new(wl_seat, id);
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

		return;
	}

	struct seat* seat = seat_find_by_id(&self->seats, id);
	if (seat) {
		wl_list_remove(&seat->link);
		seat_destroy(seat);

		/* TODO: If this is the selected seat, exit */

		return;
	}
}

void wayvnc_destroy(struct wayvnc* self)
{
	output_list_destroy(&self->outputs);
	seat_list_destroy(&self->seats);

	zxdg_output_manager_v1_destroy(self->xdg_output_manager);

	wl_shm_destroy(self->screencopy_backend.wl_shm);

	zwp_virtual_keyboard_v1_destroy(self->keyboard_backend.virtual_keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(self->keyboard_manager);
	keyboard_destroy(&self->keyboard_backend);

	zwlr_virtual_pointer_manager_v1_destroy(self->pointer_manager);
	pointer_destroy(&self->pointer_backend);

	if (self->screencopy_backend.manager)
		zwlr_screencopy_manager_v1_destroy(self->screencopy_backend.manager);

	if (self->dmabuf_backend.manager)
		zwlr_export_dmabuf_manager_v1_destroy(self->dmabuf_backend.manager);

	wl_display_disconnect(self->display);
}

static void init_xdg_outputs(struct wayvnc* self)
{
	struct output* output;
	wl_list_for_each(output, &self->outputs, link) {
		struct zxdg_output_v1* xdg_output =
			zxdg_output_manager_v1_get_xdg_output(
				self->xdg_output_manager, output->wl_output);

		output_set_xdg_output(output, xdg_output);
	}
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

	wl_display_dispatch(self->display);
	wl_display_roundtrip(self->display);

	init_xdg_outputs(self);

	if (!self->pointer_manager) {
		log_error("Virtual Pointer protocol not supported by compositor.\n");
		goto failure;
	}

	if (!self->keyboard_manager) {
		log_error("Virtual Keyboard protocol not supported by compositor.\n");
		goto failure;
	}

	wl_display_dispatch(self->display);
	wl_display_roundtrip(self->display);

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

void on_wayland_event(void* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);

	if (wl_display_prepare_read(self->display) < 0) {
		wl_display_cancel_read(self->display);
		return;
	}

	if (wl_display_read_events(self->display) < 0 && errno == EPIPE) {
		log_error("Compositor has gone away. Exiting...\n");
		wayvnc_exit(self);
	}

	wl_display_dispatch_pending(self->display);
}

void wayvnc_exit(struct wayvnc* self)
{
	self->do_exit = true;
}

void on_signal(void* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);
	wayvnc_exit(self);
}

int init_main_loop(struct wayvnc* self)
{
	struct aml* loop = aml_get_default();

	struct aml_handler* wl_handler;
	wl_handler = aml_handler_new(wl_display_get_fd(self->display),
	                             on_wayland_event, self, NULL);
	if (!wl_handler)
		return -1;

	int rc = aml_start(loop, wl_handler);
	aml_unref(wl_handler);
	if (rc < 0)
		return -1;

	struct aml_signal* sig;
	sig = aml_signal_new(SIGINT, on_signal, self, NULL);
	if (!sig)
		return -1;

	rc = aml_start(loop, sig);
	aml_unref(sig);
	if (rc < 0)
		return -1;

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

	uint32_t xfx = 0, xfy = 0;
	output_transform_coord(wayvnc->selected_output, x, y, &xfx, &xfy);

	pointer_set(&wayvnc->pointer_backend, xfx, xfy, button_mask);
}

static void on_key_event(struct nvnc_client* client, uint32_t symbol,
                         bool is_pressed)
{
	struct nvnc* nvnc = nvnc_get_server(client);
	struct wayvnc* wayvnc = nvnc_get_userdata(nvnc);

	keyboard_feed(&wayvnc->keyboard_backend, symbol, is_pressed);
}

bool on_auth(const char* username, const char* password, void* ud)
{
	struct wayvnc* self = ud;

	if (strcmp(username, self->cfg.username) != 0)
		return false;

	if (strcmp(password, self->cfg.password) != 0)
		return false;

	return true;
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
			    output_get_transformed_width(self->selected_output),
			    output_get_transformed_height(self->selected_output),
			    format);

	if (self->cfg.enable_auth)
		nvnc_enable_auth(self->nvnc, self->cfg.private_key_file,
		                 self->cfg.certificate_file, on_auth, self);

	if (self->pointer_manager)
		nvnc_set_pointer_fn(self->nvnc, on_pointer_event);

	if (self->keyboard_backend.virtual_keyboard)
		nvnc_set_key_fn(self->nvnc, on_key_event);

	return 0;
}

int wayvnc_start_capture(struct wayvnc* self)
{
	return frame_capture_start(self->capture_backend);
}

static void on_damage_check_done(struct pixman_region16* damage, void* userdata)
{
	struct wayvnc* self = userdata;

	if (pixman_region_not_empty(damage))
		nvnc_feed_frame(self->nvnc, self->current_fb, damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit(self);
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

		uint32_t tfx0, tfy0, tfx1, tfy1;
		output_transform_box_coord(self->selected_output,
		                           hint_x, hint_y,
		                           hint_x + hint_width,
		                           hint_y + hint_height,
		                           &tfx0, &tfy0, &tfx1, &tfy1);

		struct pixman_box16 hint = {
			.x1 = tfx0, .y1 = tfy0,
			.x2 = tfx1, .y2 = tfy1,
		};

		uint8_t* damage_buffer = malloc(width * height);
		render_damage(&self->renderer);
		renderer_read_damage(&self->renderer, damage_buffer, 0, height);

		damage_check_async(damage_buffer, width, height, &hint,
		                   on_damage_check_done, self);
		return;
	}

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, width, height);
	nvnc_feed_frame(self->nvnc, self->current_fb, &damage);
	pixman_region_fini(&damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit(self);
	}
}

void wayvnc_process_frame(struct wayvnc* self)
{
	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);
	uint32_t fb_width = output_get_transformed_width(self->selected_output);
	uint32_t fb_height =
		output_get_transformed_height(self->selected_output);

	struct nvnc_fb* fb = nvnc_fb_new(fb_width, fb_height, format);
	void* addr = nvnc_fb_get_addr(fb);

	frame_capture_render(self->capture_backend, &self->renderer, fb);
	renderer_read_frame(&self->renderer, addr, 0, fb_height);

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
		wayvnc_exit(self);
		break;
	case CAPTURE_FAILED:
		if (wayvnc_start_capture(self) < 0) {
			log_error("Failed to start capture. Exiting...\n");
			wayvnc_exit(self);
		}
		break;
	case CAPTURE_DONE:
		wayvnc_process_frame(self);
		break;
	}
}

int wayvnc_usage(FILE* stream, int rc)
{
	static const char* usage =
"Usage: wayvnc [options] [address [port]]\n"
"\n"
"    -C,--config=<path>                        Select a config file.\n"
"    -c,--frame-capturing=screencopy|dmabuf    Select frame capturing backend.\n"
"    -o,--output=<name>                        Select output to capture.\n"
"    -k,--keyboard=<layout>                    Select keyboard layout.\n"
"    -s,--seat=<name>                          Select seat by name.\n"
"    -r,--render-cursor                        Enable overlay cursor rendering.\n"
"    -h,--help                                 Get help (this text).\n"
"\n";

	fprintf(stream, "%s", usage);

	return rc;
}

int check_cfg_sanity(struct cfg* cfg)
{
	if (cfg->enable_auth) {
		int rc = 0;

		if (!nvnc_has_auth()) {
			log_error("Authentication can't be enabled because it was not selected during build\n");
			return -1;
		}

		if (!cfg->certificate_file) {
			log_error("Authentication enabled, but missing certificate_file\n");
			rc = -1;
		}

		if (!cfg->private_key_file) {
			log_error("Authentication enabled, but missing private_key_file\n");
			rc = -1;
		}

		if (!cfg->username) {
			log_error("Authentication enabled, but missing username\n");
			rc = -1;
		}

		if (!cfg->password) {
			log_error("Authentication enabled, but missing password\n");
			rc = -1;
		}

		return rc;
	}

	return 0;
}

int main(int argc, char* argv[])
{
	struct wayvnc self = { 0 };

	const char* cfg_file = NULL;

	const char* address = NULL;
	int port = 0;

	const char* output_name = NULL;
	enum frame_capture_backend_type fcbackend = FRAME_CAPTURE_BACKEND_NONE;
	const char* seat_name = NULL;
	
	bool overlay_cursor = false;

	static const char* shortopts = "C:c:o:k:s:rh";

	static const struct option longopts[] = {
		{ "config", required_argument, NULL, 'C' },
		{ "frame-capturing", required_argument, NULL, 'c' },
		{ "output", required_argument, NULL, 'o' },
		{ "keyboard", required_argument, NULL, 'k' },
		{ "seat", required_argument, NULL, 's' },
		{ "render-cursor", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'C':
			cfg_file = optarg;
			break;
		case 'c':
			fcbackend = frame_capture_backend_from_string(optarg);
			if (fcbackend == FRAME_CAPTURE_BACKEND_NONE) {
				fprintf(stderr, "Invalid backend: %s\n\n",
					optarg);

				return wayvnc_usage(stderr, 1);
			}
			break;
		case 'o':
			output_name = optarg;
			break;
		case 'k':
			self.kb_layout = optarg;
			break;
		case 's':
			seat_name = optarg;
			break;
		case 'r':
			overlay_cursor = true;
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

	errno = 0;
	int cfg_rc = cfg_load(&self.cfg, cfg_file);
	if (cfg_rc != 0 && (cfg_file || errno != ENOENT)) {
		if (cfg_rc > 0) {
			log_error("Failed to load config. Error on line %d\n",
			          cfg_rc);
		} else {
			log_error("Failed to load config. %m\n");
		}

		return 1;
	}

	if (check_cfg_sanity(&self.cfg) < 0)
		return 1;

	if (cfg_rc == 0) {
		if (!address) address = self.cfg.address;
		if (!port) port = self.cfg.port;
	}

	if (!address) address = DEFAULT_ADDRESS;
	if (!port) port = DEFAULT_PORT;

	if (init_wayland(&self) < 0) {
		log_error("Failed to initialise wayland\n");
		return 1;
	}

	struct output* out;
	if (output_name) {
		out = output_find_by_name(&self.outputs, output_name);
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

	self.keyboard_backend.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			self.keyboard_manager, self.selected_seat->wl_seat);

	keyboard_init(&self.keyboard_backend, self.kb_layout);

	self.pointer_backend.vnc = self.nvnc;
	self.pointer_backend.output = self.selected_output;

	self.pointer_backend.pointer = self.pointer_manager_version == 2
		? zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
			self.pointer_manager, self.selected_seat->wl_seat,
			out->wl_output)
		: zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
			self.pointer_manager, self.selected_seat->wl_seat);

	pointer_init(&self.pointer_backend);

	enum renderer_input_type renderer_input_type =
		fcbackend == FRAME_CAPTURE_BACKEND_DMABUF ?
			RENDERER_INPUT_DMABUF : RENDERER_INPUT_FB;
	if (renderer_init(&self.renderer, self.selected_output,
	                  renderer_input_type) < 0) {
		log_error("Failed to initialise renderer\n");
		goto failure;
	}

	struct aml* aml = aml_new(NULL, 0);
	if (!aml)
		goto main_loop_failure;

	aml_set_default(aml);

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

	self.capture_backend->overlay_cursor = overlay_cursor;

	if (wayvnc_start_capture(&self) < 0)
		goto capture_failure;

	wl_display_dispatch(self.display);

	while (!self.do_exit) {
		wl_display_flush(self.display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	frame_capture_stop(self.capture_backend);

	if (self.current_fb) nvnc_fb_unref(self.current_fb);
	if (self.last_fb) nvnc_fb_unref(self.last_fb);

	nvnc_close(self.nvnc);
	renderer_destroy(&self.renderer);
	screencopy_destroy(&self.screencopy_backend);
	wayvnc_destroy(&self);
	aml_unref(aml);

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
