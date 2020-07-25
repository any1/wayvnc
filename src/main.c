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
#include <wayland-client.h>
#include <pixman.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gbm.h>
#include <xf86drm.h>

#include "wlr-screencopy-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "virtual-keyboard-unstable-v1.h"
#include "xdg-output-unstable-v1.h"
#include "linux-dmabuf-unstable-v1.h"
#include "screencopy.h"
#include "strlcpy.h"
#include "logging.h"
#include "output.h"
#include "pointer.h"
#include "keyboard.h"
#include "seat.h"
#include "cfg.h"
#include "pixman-renderer.h"
#include "transform-util.h"
#include "damage-refinery.h"
#include "usdt.h"

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

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

	const struct output* selected_output;
	const struct seat* selected_seat;

	struct screencopy screencopy;
	struct pointer pointer_backend;
	struct keyboard keyboard_backend;

	struct aml_handler* wayland_handler;
	struct aml_signal* signal_handler;

	struct nvnc* nvnc;
	struct nvnc_display* nvnc_display;
	struct nvnc_fb* buffer;

	struct damage_refinery damage_refinery;
	struct pixman_region16 current_damage;

	const char* kb_layout;

	uint32_t damage_area_sum;
	uint32_t n_frames_captured;
	uint32_t n_frames_rendered;
};

void wayvnc_exit(struct wayvnc* self);
void on_capture_done(struct screencopy* sc);
static void on_render(struct nvnc_display* display, struct nvnc_fb* fb);

struct wl_shm* wl_shm = NULL;
struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf = NULL;
struct gbm_device* gbm_device = NULL;

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
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
		return;
	}

	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		self->screencopy.manager =
			wl_registry_bind(registry, id,
					 &zwlr_screencopy_manager_v1_interface,
					 version);
		self->screencopy.version = version;
		return;
	}

	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		self->pointer_manager =
			wl_registry_bind(registry, id,
					 &zwlr_virtual_pointer_manager_v1_interface,
					 version);
		self->pointer_manager_version = version;
		return;
	};

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

	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		zwp_linux_dmabuf = wl_registry_bind(registry, id,
				&zwp_linux_dmabuf_v1_interface, 3);
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

		if (out == self->selected_output) {
			log_error("Selected output went away. Exiting...\n");
			wayvnc_exit(self);
		}

		return;
	}

	struct seat* seat = seat_find_by_id(&self->seats, id);
	if (seat) {
		wl_list_remove(&seat->link);
		seat_destroy(seat);

		if (seat == self->selected_seat) {
			log_error("Selected seat went away. Exiting...\n");
			wayvnc_exit(self);
		}

		return;
	}
}

static int find_render_node(char *node, size_t maxlen) {
	bool r = -1;
	drmDevice *devices[64];

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		strlcpy(node, dev->nodes[DRM_NODE_RENDER], maxlen);
		r = 0;
		break;
	}

	drmFreeDevices(devices, n);
	return r;
}

void wayvnc_destroy(struct wayvnc* self)
{
	output_list_destroy(&self->outputs);
	seat_list_destroy(&self->seats);

	zxdg_output_manager_v1_destroy(self->xdg_output_manager);

	wl_shm_destroy(wl_shm);

	zwp_virtual_keyboard_v1_destroy(self->keyboard_backend.virtual_keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(self->keyboard_manager);
	keyboard_destroy(&self->keyboard_backend);

	zwlr_virtual_pointer_manager_v1_destroy(self->pointer_manager);
	pointer_destroy(&self->pointer_backend);

	if (self->screencopy.manager)
		zwlr_screencopy_manager_v1_destroy(self->screencopy.manager);

	wl_registry_destroy(self->registry);
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

	if (!self->screencopy.manager) {
		log_error("Compositor doesn't support screencopy! Exiting.\n");
		goto failure;
	}

	self->screencopy.on_done = on_capture_done;
	self->screencopy.userdata = self;

	return 0;

failure:
	wl_display_disconnect(self->display);
	return -1;
}

void on_wayland_event(void* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);

	int rc = wl_display_prepare_read(self->display);
	assert(rc == 0);

	if (wl_display_read_events(self->display) < 0) {
		if (errno == EPIPE) {
			log_error("Compositor has gone away. Exiting...\n");
			wayvnc_exit(self);
		} else {
			log_error("Failed to read wayland events: %m\n");
		}
	}

	if (wl_display_dispatch_pending(self->display) < 0)
		log_error("Failed to dispatch pending\n");
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

static void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
			     enum nvnc_button_mask button_mask)
{
	// TODO: Have a seat per client

	struct nvnc* nvnc = nvnc_client_get_server(client);
	struct wayvnc* wayvnc = nvnc_get_userdata(nvnc);

	uint32_t xfx = 0, xfy = 0;
	output_transform_coord(wayvnc->selected_output, x, y, &xfx, &xfy);

	pointer_set(&wayvnc->pointer_backend, xfx, xfy, button_mask);
}

static void on_key_event(struct nvnc_client* client, uint32_t symbol,
                         bool is_pressed)
{
	struct nvnc* nvnc = nvnc_client_get_server(client);
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
	if (!self->nvnc) {
		log_error("Failed to bind to address\n");
		return -1;
	}

	self->nvnc_display = nvnc_display_new(0, 0);
	if (!self->nvnc_display)
		goto failure;

	nvnc_add_display(self->nvnc, self->nvnc_display);

	nvnc_set_userdata(self->nvnc, self);

	nvnc_set_name(self->nvnc, "WayVNC");

	nvnc_display_set_render_fn(self->nvnc_display, on_render);

	if (self->cfg.enable_auth)
		nvnc_enable_auth(self->nvnc, self->cfg.private_key_file,
		                 self->cfg.certificate_file, on_auth, self);

	if (self->pointer_manager)
		nvnc_set_pointer_fn(self->nvnc, on_pointer_event);

	if (self->keyboard_backend.virtual_keyboard)
		nvnc_set_key_fn(self->nvnc, on_key_event);

	return 0;

failure:
	nvnc_close(self->nvnc);
	return -1;
}

int wayvnc_start_capture(struct wayvnc* self)
{
	int rc = screencopy_start(&self->screencopy);
	if (rc < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit(self);
	}
	return rc;
}

int wayvnc_start_capture_immediate(struct wayvnc* self)
{
	int rc = screencopy_start_immediate(&self->screencopy);
	if (rc < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit(self);
	}
	return rc;
}

static void on_render(struct nvnc_display* display, struct nvnc_fb* fb)
{
	struct nvnc* nvnc = nvnc_display_get_server(display);
	struct wayvnc* self = nvnc_get_userdata(nvnc);

	if (!self->screencopy.back)
		return;

	self->n_frames_rendered++;

	DTRACE_PROBE(wayvnc, render_start);

	enum wl_output_transform transform = self->selected_output->transform;
	wv_pixman_render(fb, self->screencopy.back, transform,
			&self->current_damage);
	pixman_region_clear(&self->current_damage);

	DTRACE_PROBE(wayvnc, render_end);
}

static void wayvnc_damage_region(struct wayvnc* self,
                                 struct pixman_region16* damage)
{
	pixman_region_union(&self->current_damage, &self->current_damage, damage);
	nvnc_display_damage_region(self->nvnc_display, damage);
}

// TODO: Handle transform change too
void on_output_dimension_change(struct output* output)
{
	struct wayvnc* self = output->userdata;
	assert(self->selected_output == output);

	// The buffer must have been set as a result of a screencopy frame
	if (!self->buffer)
		return;

	log_debug("Output dimensions changed. Restarting frame capturer...\n");

	screencopy_stop(&self->screencopy);
	wayvnc_start_capture_immediate(self);
}

static uint32_t calculate_region_area(struct pixman_region16* region)
{
	uint32_t area = 0;

	int n_rects = 0;
	struct pixman_box16* rects = pixman_region_rectangles(region,
		&n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int width = rects[i].x2 - rects[i].x1;
		int height = rects[i].y2 - rects[i].y1;
		area += width * height;
	}

	return area;
}

void wayvnc_process_frame(struct wayvnc* self)
{
	uint32_t width = output_get_transformed_width(self->selected_output);
	uint32_t height = output_get_transformed_height(self->selected_output);
	uint32_t format = self->screencopy.back->format;

	if ((int)self->selected_output->width != self->screencopy.back->width
	   || (int)self->selected_output->height != self->screencopy.back->height) {
		log_debug("Frame dimensions don't match output. Recapturing frame...\n");
		wayvnc_start_capture_immediate(self);
		return;
	}

	if (self->buffer && (width != nvnc_fb_get_width(self->buffer) ||
			height != nvnc_fb_get_height(self->buffer))) {
		log_debug("Frame dimensions changed. Resizing...\n");

		nvnc_fb_unref(self->buffer);
		self->buffer = NULL;
		damage_refinery_destroy(&self->damage_refinery);
	}

	// TODO: Maybe the buffer needs to be changed inside on_render?
	if (!self->buffer) {
		self->buffer = nvnc_fb_new(width, height, format);
		nvnc_display_set_buffer(self->nvnc_display, self->buffer);
		log_debug("Set nvnc buffer: %p\n", self->buffer);

		damage_refinery_init(&self->damage_refinery,
			self->screencopy.back->width,
			self->screencopy.back->height);
	}

	self->n_frames_captured++;
	self->damage_area_sum +=
		calculate_region_area(&self->screencopy.back->damage);

	DTRACE_PROBE(wayvnc, refine_damage_start);

	struct pixman_region16 txdamage, refined;
	pixman_region_init(&txdamage);
	pixman_region_init(&refined);
	damage_refine(&self->damage_refinery, &refined,
			&self->screencopy.back->damage,
			self->screencopy.back);
	wv_region_transform(&txdamage, &refined,
			self->selected_output->transform,
			self->selected_output->width,
			self->selected_output->height);
//	damage_dump(stdout, &txdamage, width, height, 32);
	wayvnc_damage_region(self, &txdamage);
	pixman_region_fini(&refined);
	pixman_region_fini(&txdamage);

	DTRACE_PROBE(wayvnc, refine_damage_end);

	wayvnc_start_capture(self);
}

void on_capture_done(struct screencopy* sc)
{
	struct wayvnc* self = sc->userdata;

	switch (sc->status) {
	case SCREENCOPY_STOPPED:
		break;
	case SCREENCOPY_IN_PROGRESS:
		break;
	case SCREENCOPY_FATAL:
		log_error("Fatal error while capturing. Exiting...\n");
		wayvnc_exit(self);
		break;
	case SCREENCOPY_FAILED:
		wayvnc_start_capture_immediate(self);
		break;
	case SCREENCOPY_DONE:
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
"    -o,--output=<name>                        Select output to capture.\n"
"    -k,--keyboard=<layout>                    Select keyboard layout.\n"
"    -s,--seat=<name>                          Select seat by name.\n"
"    -r,--render-cursor                        Enable overlay cursor rendering.\n"
"    -f,--max-fps=<fps>                        Set the rate limit (default 30).\n"
"    -p,--show-performance                     Show performance counters.\n"
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

static void on_perf_tick(void* obj)
{
	struct wayvnc* self = aml_get_userdata(obj);

	double total_area = self->selected_output->width * self->selected_output->height;
	double area_avg = (double)self->damage_area_sum / (double)self->n_frames_captured;
	double relative_area_avg = 100.0 * area_avg / total_area;

	printf("Frames captured: %"PRIu32", rendered: %"PRIu32", average reported frame damage: %.1f %%\n",
			self->n_frames_captured, self->n_frames_rendered,
			relative_area_avg);

	self->n_frames_captured = 0;
	self->n_frames_rendered = 0;
	self->damage_area_sum = 0;
}

static void start_performance_ticker(struct wayvnc* self)
{
	struct aml_ticker* ticker = aml_ticker_new(1000, on_perf_tick, self,
		NULL);
	if (!ticker)
		return;

	aml_start(aml_get_default(), ticker);
	aml_unref(ticker);
}

int main(int argc, char* argv[])
{
	struct wayvnc self = { 0 };

	const char* cfg_file = NULL;

	const char* address = NULL;
	int port = 0;

	const char* output_name = NULL;
	const char* seat_name = NULL;
	
	bool overlay_cursor = false;
	bool show_performance = false;
	int max_rate = 30;

	static const char* shortopts = "C:o:k:s:rf:hp";
	int drm_fd = -1;

	static const struct option longopts[] = {
		{ "config", required_argument, NULL, 'C' },
		{ "output", required_argument, NULL, 'o' },
		{ "keyboard", required_argument, NULL, 'k' },
		{ "seat", required_argument, NULL, 's' },
		{ "render-cursor", no_argument, NULL, 'r' },
		{ "max-fps", required_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ "show-performance", no_argument, NULL, 'p' },
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
		case 'f':
			max_rate = atoi(optarg);
			break;
		case 'p':
			show_performance = true;
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
	self.screencopy.wl_output = out->wl_output;
	self.screencopy.rate_limit = max_rate;

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

	out->on_dimension_change = on_output_dimension_change;
	out->userdata = &self;

	char render_node[256];
	if (find_render_node(render_node, sizeof(render_node)) < 0)
		goto failure;

	drm_fd = open(render_node, O_RDWR);
	if (drm_fd < 0)
		goto failure;

	gbm_device = gbm_create_device(drm_fd);
	if (!gbm_device)
		goto failure;

	struct aml* aml = aml_new();
	if (!aml)
		goto main_loop_failure;

	aml_set_default(aml);

	if (init_main_loop(&self) < 0)
		goto main_loop_failure;

	if (init_nvnc(&self, address, port) < 0)
		goto nvnc_failure;

	if (self.screencopy.manager)
		screencopy_init(&self.screencopy);

	if (!self.screencopy.manager) {
		log_error("screencopy is not supported by compositor\n");
		goto capture_failure;
	}

	pixman_region_init(&self.current_damage);

	self.screencopy.overlay_cursor = overlay_cursor;

	if (wayvnc_start_capture(&self) < 0)
		goto capture_failure;

	if (show_performance)
		start_performance_ticker(&self);

	wl_display_dispatch(self.display);

	while (!self.do_exit) {
		wl_display_flush(self.display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	screencopy_stop(&self.screencopy);

	if (self.buffer) {
		damage_refinery_destroy(&self.damage_refinery);
		nvnc_fb_unref(self.buffer);
	}

	pixman_region_fini(&self.current_damage);

	nvnc_display_unref(self.nvnc_display);
	nvnc_close(self.nvnc);
	if (zwp_linux_dmabuf)
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf);
	if (self.screencopy.manager)
		screencopy_destroy(&self.screencopy);
	gbm_device_destroy(gbm_device);
	wayvnc_destroy(&self);
	aml_unref(aml);

	return 0;

capture_failure:
	nvnc_display_unref(self.nvnc_display);
	nvnc_close(self.nvnc);
nvnc_failure:
main_loop_failure:
failure:
	gbm_device_destroy(gbm_device);
	if (drm_fd >= 0)
		close(drm_fd);
	wayvnc_destroy(&self);
	return 1;
}
