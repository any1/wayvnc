/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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
#include "wlr-virtual-pointer-unstable-v1.h"
#include "virtual-keyboard-unstable-v1.h"
#include "xdg-output-unstable-v1.h"
#include "wlr-output-power-management-unstable-v1.h"
#include "linux-dmabuf-unstable-v1.h"
#include "screencopy.h"
#include "data-control.h"
#include "strlcpy.h"
#include "output.h"
#include "pointer.h"
#include "keyboard.h"
#include "seat.h"
#include "cfg.h"
#include "transform-util.h"
#include "usdt.h"
#include "ctl-server.h"
#include "util.h"
#include "option-parser.h"

#ifdef ENABLE_PAM
#include "pam_auth.h"
#endif

#ifdef ENABLE_SCREENCOPY_DMABUF
#include <gbm.h>
#include <xf86drm.h>
#endif

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 5900

#define MAYBE_UNUSED __attribute__((unused))

struct wayvnc {
	bool do_exit;

	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_list outputs;
	struct wl_list seats;
	struct cfg cfg;

	struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
	struct zwlr_virtual_pointer_manager_v1* pointer_manager;
	struct zwlr_data_control_manager_v1* data_control_manager;

	struct output* selected_output;
	const struct seat* selected_seat;

	struct screencopy screencopy;

	struct aml_handler* wayland_handler;
	struct aml_signal* signal_handler;

	struct nvnc* nvnc;
	struct nvnc_display* nvnc_display;

	const char* kb_layout;
	const char* kb_variant;

	uint32_t damage_area_sum;
	uint32_t n_frames_captured;

	bool disable_input;

	int nr_clients;
	struct aml_ticker* performance_ticker;

	struct aml_timer* capture_retry_timer;

	struct ctl* ctl;
	bool is_initializing;
};

struct wayvnc_client {
	struct wayvnc* server;
	struct nvnc_client* nvnc_client;

	unsigned id;
	struct pointer pointer;
	struct keyboard keyboard;
	struct data_control data_control;
};

void wayvnc_exit(struct wayvnc* self);
void on_capture_done(struct screencopy* sc);
static void on_nvnc_client_new(struct nvnc_client* client);
void switch_to_output(struct wayvnc*, struct output*);
void switch_to_next_output(struct wayvnc*);
void switch_to_prev_output(struct wayvnc*);
static void client_init_pointer(struct wayvnc_client* self);
static void client_init_keyboard(struct wayvnc_client* self);
static void client_init_data_control(struct wayvnc_client* self);

struct wl_shm* wl_shm = NULL;
struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf = NULL;
struct gbm_device* gbm_device = NULL;
struct zxdg_output_manager_v1* xdg_output_manager = NULL;
struct zwlr_output_power_manager_v1* wlr_output_power_manager = NULL;

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
			wl_seat_destroy(wl_seat);
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
		}
		return;
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new xdg_output_manager");
		xdg_output_manager =
			wl_registry_bind(registry, id,
			                 &zxdg_output_manager_v1_interface, 3);

		output_setup_wl_managers(&self->outputs);
		return;
	}

	if (strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new wlr_output_power_manager");
		wlr_output_power_manager = wl_registry_bind(registry, id,
					&zwlr_output_power_manager_v1_interface, 1);
		output_setup_wl_managers(&self->outputs);
		return;
	}

	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		self->screencopy.manager =
			wl_registry_bind(registry, id,
					 &zwlr_screencopy_manager_v1_interface,
					 MIN(3, version));
		return;
	}

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

		wl_list_remove(&out->link);
		output_destroy(out);

		if (out == self->selected_output) {
			nvnc_log(NVNC_LOG_ERROR, "No fallback outputs left. Exiting...");
			wayvnc_exit(self);
		}

		return;
	}

	struct seat* seat = seat_find_by_id(&self->seats, id);
	if (seat) {
		nvnc_log(NVNC_LOG_INFO, "Seat %s went away", seat->name);
		wl_list_remove(&seat->link);
		seat_destroy(seat);

		if (seat == self->selected_seat) {
			nvnc_log(NVNC_LOG_ERROR, "Selected seat went away. Exiting...");
			wayvnc_exit(self);
		}

		return;
	}
}

#ifdef ENABLE_SCREENCOPY_DMABUF
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

static int init_render_node(int* fd)
{
	char render_node[256];
	if (find_render_node(render_node, sizeof(render_node)) < 0)
		return -1;

	*fd = open(render_node, O_RDWR);
	if (*fd < 0)
		return -1;

	gbm_device = gbm_create_device(*fd);
	if (!gbm_device) {
		close(*fd);
		return -1;
	}

	return 0;
}
#endif

void wayvnc_destroy(struct wayvnc* self)
{
	cfg_destroy(&self->cfg);

	output_list_destroy(&self->outputs);
	seat_list_destroy(&self->seats);

	if (xdg_output_manager)
		zxdg_output_manager_v1_destroy(xdg_output_manager);

	if (wlr_output_power_manager)
		zwlr_output_power_manager_v1_destroy(wlr_output_power_manager);

	wl_shm_destroy(wl_shm);

	if (self->keyboard_manager)
		zwp_virtual_keyboard_manager_v1_destroy(self->keyboard_manager);

	if (self->pointer_manager)
		zwlr_virtual_pointer_manager_v1_destroy(self->pointer_manager);

	if (self->data_control_manager)
		zwlr_data_control_manager_v1_destroy(self->data_control_manager);

	if (self->screencopy.manager)
		zwlr_screencopy_manager_v1_destroy(self->screencopy.manager);

	if (self->performance_ticker)
		aml_unref(self->performance_ticker);

	if (self->capture_retry_timer)
		aml_unref(self->capture_retry_timer);

	wl_registry_destroy(self->registry);
	wl_display_disconnect(self->display);
}

static int init_wayland(struct wayvnc* self)
{
	self->is_initializing = true;
	static const struct wl_registry_listener registry_listener = {
		.global = registry_add,
		.global_remove = registry_remove,
	};

	self->display = wl_display_connect(NULL);
	if (!self->display) {
		const char* display_name = getenv("WAYLAND_DISPLAY");
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

	if (!self->screencopy.manager) {
		nvnc_log(NVNC_LOG_ERROR, "Screencopy protocol not supported by compositor. Exiting. Refer to FAQ section in man page.");
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

	int rc MAYBE_UNUSED = wl_display_prepare_read(self->display);
	assert(rc == 0);

	if (wl_display_read_events(self->display) < 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			nvnc_log(NVNC_LOG_ERROR, "Compositor has gone away. Exiting...");
			wayvnc_exit(self);
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to read wayland events: %m");
		}
	}

	if (wl_display_dispatch_pending(self->display) < 0)
		nvnc_log(NVNC_LOG_ERROR, "Failed to dispatch pending");
}

void wayvnc_exit(struct wayvnc* self)
{
	self->do_exit = true;
}

void on_signal(void* obj)
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
	if (!output_name || output_name[0] == '\0')
		return cmd_failed("Output name is required");
	struct output* output = output_find_by_name(&self->outputs, output_name);
	if (!output) {
		return cmd_failed("No such output \"%s\"", output_name);
	}
	switch_to_output(self, output);
	return cmd_ok();
}

static int get_client_list(struct ctl* ctl,
		struct ctl_server_vnc_client** clients)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
	if (self->nr_clients == 0) {
		*clients = NULL;
		return 0;
	}
	*clients = calloc(self->nr_clients, sizeof(**clients));
	struct nvnc_client* nvnc_client = nvnc_client_first(self->nvnc);
	for (int i = 0; i < self->nr_clients && nvnc_client; ++i) {
		struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);
		struct ctl_server_vnc_client* ctl_client =&(*clients)[i];

		snprintf(ctl_client->id, sizeof(ctl_client->id), "%u",
				client->id);
		const char* hostname = nvnc_client_get_hostname(nvnc_client);
		if (hostname)
			strlcpy(ctl_client->hostname, hostname,
					sizeof(ctl_client->hostname));
		const char* username = nvnc_client_get_auth_username(nvnc_client);
		if (username)
			strlcpy(ctl_client->username, username,
					sizeof(ctl_client->username));
		nvnc_client = nvnc_client_next(nvnc_client);
	}
	return self->nr_clients;
}

static int get_output_list(struct ctl* ctl,
		struct ctl_server_output** outputs)
{
	struct wayvnc* self = ctl_server_userdata(ctl);
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
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);
	struct wayvnc* wayvnc = wv_client->server;

	uint32_t xfx = 0, xfy = 0;
	output_transform_coord(wayvnc->selected_output, x, y, &xfx, &xfy);

	pointer_set(&wv_client->pointer, xfx, xfy, button_mask);
}

static void on_key_event(struct nvnc_client* client, uint32_t symbol,
                         bool is_pressed)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);

	keyboard_feed(&wv_client->keyboard, symbol, is_pressed);
}

static void on_key_code_event(struct nvnc_client* client, uint32_t code,
		bool is_pressed)
{
	struct wayvnc_client* wv_client = nvnc_get_userdata(client);

	keyboard_feed_code(&wv_client->keyboard, code + 8, is_pressed);
}

static void on_client_cut_text(struct nvnc_client* nvnc_client,
		const char* text, uint32_t len)
{
	struct wayvnc_client* client = nvnc_get_userdata(nvnc_client);

	if (client->data_control.manager) {
		data_control_to_clipboard(&client->data_control, text, len);
	}
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
	struct nvnc_fb* placeholder_fb =
		create_placeholder_buffer(self->selected_output->width,
				self->selected_output->height);
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

int init_nvnc(struct wayvnc* self, const char* addr, uint16_t port, bool is_unix)
{
	self->nvnc = is_unix ? nvnc_open_unix(addr) : nvnc_open(addr, port);
	if (!self->nvnc) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to bind to address. Add -Ldebug to the argument list for more info.");
		return -1;
	}
	if (is_unix)
		nvnc_log(NVNC_LOG_INFO, "Listening for connections on %s", addr);
	else
		nvnc_log(NVNC_LOG_INFO, "Listening for connections on %s:%d", addr, port);

	self->nvnc_display = nvnc_display_new(0, 0);
	if (!self->nvnc_display)
		goto failure;

	nvnc_add_display(self->nvnc, self->nvnc_display);

	nvnc_set_userdata(self->nvnc, self, NULL);

	nvnc_set_name(self->nvnc, "WayVNC");

	if (self->cfg.enable_auth &&
	    nvnc_enable_auth(self->nvnc, self->cfg.private_key_file,
	                     self->cfg.certificate_file, on_auth, self) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to enable authentication");
		goto failure;
	}

	if (self->pointer_manager)
		nvnc_set_pointer_fn(self->nvnc, on_pointer_event);

	if (self->keyboard_manager) {
		nvnc_set_key_fn(self->nvnc, on_key_event);
		nvnc_set_key_code_fn(self->nvnc, on_key_code_event);
	}

	nvnc_set_new_client_fn(self->nvnc, on_nvnc_client_new);
	nvnc_set_cut_text_fn(self->nvnc, on_client_cut_text);

	if (blank_screen(self) != 0)
		goto failure;

	return 0;

failure:
	nvnc_close(self->nvnc);
	return -1;
}

int wayvnc_start_capture(struct wayvnc* self)
{
	int rc = screencopy_start(&self->screencopy);
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

	if (self->selected_output->power == OUTPUT_POWER_OFF) {
		nvnc_log(NVNC_LOG_WARNING, "Selected output is in powersaving mode. Delaying capture until it turns on.");
		if (output_set_power_state(self->selected_output, OUTPUT_POWER_ON)
				== 0)
			nvnc_log(NVNC_LOG_WARNING, "Requested power ON.");
		return 0;
	}

	int rc = screencopy_start_immediate(&self->screencopy);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to start capture. Exiting...");
		wayvnc_exit(self);
	}
	return rc;
}

static void on_capture_restart_timer(void* obj)
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

	screencopy_stop(&self->screencopy);
	wayvnc_start_capture_immediate(self);
}

static void on_output_power_change(struct output* output)
{
	nvnc_trace("Output %s power state changed to %s", output->name, output_power_state_name(output->power));

	struct wayvnc* self = output->userdata;
	if (self->selected_output != output || self->nr_clients == 0)
		return;

	switch (output->power) {
	case OUTPUT_POWER_ON:
		nvnc_log(NVNC_LOG_WARNING, "Output is now on. Restarting frame capture");
		wayvnc_start_capture_immediate(self);
		break;
	case OUTPUT_POWER_OFF:
		nvnc_log(NVNC_LOG_WARNING, "Output is now off. Pausing frame capture");
		screencopy_stop(&self->screencopy);
		blank_screen(self);
		break;
	default:
		break;
	}
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
	struct wv_buffer* buffer = self->screencopy.back;
	self->screencopy.back = NULL;

	self->n_frames_captured++;
	self->damage_area_sum +=
		calculate_region_area(&buffer->damage);

	struct pixman_region16 damage;
	pixman_region_init(&damage);

	enum wl_output_transform output_transform, buffer_transform;
	output_transform = self->selected_output->transform;

	if (buffer->y_inverted) {
		buffer_transform = wv_output_transform_compose(output_transform,
				WL_OUTPUT_TRANSFORM_FLIPPED_180);

		wv_region_transform(&damage, &buffer->damage,
				WL_OUTPUT_TRANSFORM_FLIPPED_180,
				buffer->width, buffer->height);
	} else {
		buffer_transform = output_transform;
		pixman_region_copy(&damage, &buffer->damage);
	}

	nvnc_fb_set_transform(buffer->nvnc_fb,
			(enum nvnc_transform)buffer_transform);

	nvnc_display_feed_buffer(self->nvnc_display, buffer->nvnc_fb,
			&damage);

	pixman_region_fini(&damage);

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
		nvnc_log(NVNC_LOG_ERROR, "Fatal error while capturing. Exiting...");
		wayvnc_exit(self);
		break;
	case SCREENCOPY_FAILED:
		wayvnc_restart_capture(self);
		break;
	case SCREENCOPY_DONE:
		wayvnc_process_frame(self);
		break;
	}
}

int wayvnc_usage(struct option_parser* parser, FILE* stream, int rc)
{
	static const char* usage =
"Usage: wayvnc [options] [<address> [<port>]]\n"
"\n"
"Starts a VNC server for $WAYLAND_DISPLAY";
	fprintf(stream, "%s\n\n", usage);
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
			return -1;
		}

		if (!cfg->certificate_file) {
			nvnc_log(NVNC_LOG_ERROR, "Authentication enabled, but missing certificate_file");
			rc = -1;
		}

		if (!cfg->private_key_file) {
			nvnc_log(NVNC_LOG_ERROR, "Authentication enabled, but missing private_key_file");
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

	nvnc_log(NVNC_LOG_INFO, "Frames captured: %"PRIu32", average reported frame damage: %.1f %%",
			self->n_frames_captured, relative_area_avg);

	self->n_frames_captured = 0;
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
	client_init_keyboard(self);
	client_init_pointer(self);
	client_init_data_control(self);

	return self;
}

static void client_destroy(void* obj)
{
	struct wayvnc_client* self = obj;

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

static void on_nvnc_client_cleanup(struct nvnc_client* client)
{
	struct wayvnc_client* wayvnc_client = nvnc_get_userdata(client);
	struct nvnc* nvnc = nvnc_client_get_server(client);
	struct wayvnc* self = nvnc_get_userdata(nvnc);

	self->nr_clients--;
	nvnc_log(NVNC_LOG_DEBUG, "Client disconnected, new client count: %d",
			self->nr_clients);

	char id[64];
	snprintf(id, sizeof(id), "%u", wayvnc_client->id);
	ctl_server_event_disconnected(self->ctl, id,
			nvnc_client_get_hostname(client),
			nvnc_client_get_auth_username(client),
			self->nr_clients);

	if (self->nr_clients == 0) {
		nvnc_log(NVNC_LOG_INFO, "Stopping screen capture");
		screencopy_stop(&self->screencopy);
		stop_performance_ticker(self);
	}
}

static void on_nvnc_client_new(struct nvnc_client* client)
{
	struct nvnc* nvnc = nvnc_client_get_server(client);
	struct wayvnc* self = nvnc_get_userdata(nvnc);

	struct wayvnc_client* wayvnc_client = client_create(self, client);
	assert(wayvnc_client);
	nvnc_set_userdata(client, wayvnc_client, client_destroy);

	if (self->nr_clients == 0) {
		nvnc_log(NVNC_LOG_INFO, "Starting screen capture");
		start_performance_ticker(self);
		wayvnc_start_capture_immediate(self);
	}
	self->nr_clients++;
	nvnc_set_client_cleanup_fn(client, on_nvnc_client_cleanup);
	nvnc_log(NVNC_LOG_DEBUG, "Client connected, new client count: %d",
			self->nr_clients);

	char id[64];
	snprintf(id, sizeof(id), "%u", wayvnc_client->id);
	ctl_server_event_connected(self->ctl, id,
			nvnc_client_get_hostname(client),
			nvnc_client_get_auth_username(client),
			self->nr_clients);
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
			wayvnc->pointer_manager, wayvnc->selected_seat->wl_seat,
			wayvnc->selected_output->wl_output)
		: zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
			wayvnc->pointer_manager, wayvnc->selected_seat->wl_seat);

	if (pointer_init(&self->pointer) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise pointer");
	}
}

static void client_init_keyboard(struct wayvnc_client* self)
{
	struct wayvnc* wayvnc = self->server;

	if (!wayvnc->keyboard_manager)
		return;

	self->keyboard.virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			wayvnc->keyboard_manager,
			wayvnc->selected_seat->wl_seat);

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
	data_control_init(&self->data_control, wayvnc->display, wayvnc->nvnc,
			wayvnc->selected_seat->wl_seat);
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

void set_selected_output(struct wayvnc* self, struct output* output) {
	if (self->selected_output) {
		self->selected_output->on_dimension_change = NULL;
	}
	self->selected_output = output;
	self->screencopy.wl_output = output->wl_output;
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
	screencopy_stop(&self->screencopy);
	set_selected_output(self, output);
	reinitialise_pointers(self);
	if (self->nr_clients > 0)
		wayvnc_start_capture_immediate(self);
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
	bool use_unix_socket = false;

	const char* output_name = NULL;
	const char* seat_name = NULL;
	const char* socket_path = NULL;
	const char* keyboard_options = NULL;

	bool overlay_cursor = false;
	bool show_performance = false;
	int max_rate = 30;
	bool disable_input = false;

	int drm_fd MAYBE_UNUSED = -1;

	int log_level = NVNC_LOG_WARNING;

	static const struct wv_option opts[] = {
		{ .positional = "address",
		  .help = "The IP address or unix socket path to listen on. Default: 127.0.0.1" },
		{ .positional = "port",
		  .help = "The TCP port to listen on. Default: 5900" },
		{ 'C', "config", "<path>",
		  "Select a config file." },
		{ 'g', "gpu", NULL,
		  "Enable features that need GPU." },
		{ 'o', "output", "<name>",
		  "Select output to capture." },
		{ 'k', "keyboard", "<layout>[-<variant>]",
		  "Select keyboard layout with an optional variant." },
		{ 's', "seat", "<name>",
		  "Select seat by name." },
		{ 'S', "socket", "<path>",
		  "Control socket path." },
		{ 'r', "render-cursor", NULL,
		  "Enable overlay cursor rendering." },
		{ 'f', "max-fps", "<fps>",
		  "Set rate limit (default 30).",
		  .default_ = "30" },
		{ 'p', "performance", NULL,
		  "Show performance counters." },
		{ 'u', "unix-socket", NULL,
		  "Create unix domain socket." },
		{ 'd', "disable-input", NULL,
		  "Disable all remote input." },
		{ 'V', "version", NULL,
		  "Show version info." },
		{ 'v', "verbose", NULL,
		  "Be more verbose. Same as setting --log-level=info" },
		{ 'L', "log-level", "<level>",
		  "Set log level. The levels are: error, warning, info, debug trace and quiet.",
		  .default_ = "warning" },
		{ 'h', "help", NULL,
		  "Get help (this text)." },
		{ '\0', NULL, NULL, NULL }
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
	show_performance = !!option_parser_get_value(&option_parser, "performance");
	use_unix_socket = !!option_parser_get_value(&option_parser, "unix");
	disable_input = !!option_parser_get_value(&option_parser, "disable-input");
	log_level = option_parser_get_value(&option_parser, "verbose")
		? NVNC_LOG_INFO : NVNC_LOG_WARNING;
	log_level = log_level_from_string(
			option_parser_get_value(&option_parser, "log-level"));
	max_rate = atoi(option_parser_get_value(&option_parser, "max-fps"));

	keyboard_options = option_parser_get_value(&option_parser, "keyboard");
	if (keyboard_options)
		parse_keyboard_option(&self, keyboard_options);

	nvnc_set_log_level(log_level);

	address = option_parser_get_value(&option_parser, "address");
	const char* port_str = option_parser_get_value(&option_parser, "port");
	if (port_str)
		port = atoi(port_str);

	if (seat_name && disable_input) {
		nvnc_log(NVNC_LOG_ERROR, "seat and disable-input are conflicting options");
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

	if (cfg_rc == 0) {
		if (!address) address = self.cfg.address;
		if (!port) port = self.cfg.port;
	}

	if (!address) address = DEFAULT_ADDRESS;
	if (!port) port = DEFAULT_PORT;

	self.disable_input = disable_input;

	if (init_wayland(&self) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise wayland");
		return 1;
	}

	struct output* out;
	if (output_name) {
		out = output_find_by_name(&self.outputs, output_name);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No such output");
			goto failure;
		}
	} else {
		out = output_first(&self.outputs);
		if (!out) {
			nvnc_log(NVNC_LOG_ERROR, "No output found");
			goto failure;
		}
	}
	set_selected_output(&self, out);

	struct seat* seat = NULL;
	if (seat_name) {
		seat = seat_find_by_name(&self.seats, seat_name);
		if (!seat) {
			nvnc_log(NVNC_LOG_ERROR, "No such seat");
			goto failure;
		}
	} else if (!self.disable_input) {
		seat = seat_first(&self.seats);
		if (!seat) {
			nvnc_log(NVNC_LOG_ERROR, "No seat found");
			goto failure;
		}
	}

	self.selected_seat = seat;
	self.screencopy.rate_limit = max_rate;
	self.screencopy.enable_linux_dmabuf = enable_gpu_features;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (init_render_node(&drm_fd) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to initialise DRM render node. No GPU acceleration will be available.");
	}
#endif

	if (aml_unstable_abi_version != AML_UNSTABLE_API)
		nvnc_log(NVNC_LOG_PANIC, "libaml is incompatible with this build of wayvnc!");

	struct aml* aml = aml_new();
	if (!aml)
		goto main_loop_failure;

	aml_set_default(aml);

	if (init_main_loop(&self) < 0)
		goto main_loop_failure;

	if (init_nvnc(&self, address, port, use_unix_socket) < 0)
		goto nvnc_failure;

	if (self.screencopy.manager)
		screencopy_init(&self.screencopy);

	if (!self.screencopy.manager) {
		nvnc_log(NVNC_LOG_ERROR, "screencopy is not supported by compositor");
		goto capture_failure;
	}

	self.screencopy.overlay_cursor = overlay_cursor;

	if (show_performance)
		self.performance_ticker = aml_ticker_new(1000000, on_perf_tick,
				&self, NULL);

	const struct ctl_server_actions ctl_actions = {
		.userdata = &self,
		.on_output_cycle = on_output_cycle,
		.on_output_switch = on_output_switch,
		.get_client_list = get_client_list,
		.get_output_list = get_output_list,
		.on_disconnect_client = on_disconnect_client,
		.on_wayvnc_exit = on_wayvnc_exit,
	};
	self.ctl = ctl_server_new(socket_path, &ctl_actions);
	if (!self.ctl)
		goto ctl_server_failure;

	wl_display_dispatch_pending(self.display);

	while (!self.do_exit) {
		wl_display_flush(self.display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	nvnc_log(NVNC_LOG_INFO, "Exiting...");

	screencopy_stop(&self.screencopy);
	ctl_server_destroy(self.ctl);

	nvnc_display_unref(self.nvnc_display);
	nvnc_close(self.nvnc);
	if (zwp_linux_dmabuf)
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf);
	if (self.screencopy.manager)
		screencopy_destroy(&self.screencopy);
#ifdef ENABLE_SCREENCOPY_DMABUF
	if (gbm_device) {
		gbm_device_destroy(gbm_device);
		close(drm_fd);
	}
#endif
	wayvnc_destroy(&self);
	aml_unref(aml);

	return 0;

ctl_server_failure:
capture_failure:
	nvnc_display_unref(self.nvnc_display);
	nvnc_close(self.nvnc);
nvnc_failure:
	aml_unref(aml);
main_loop_failure:
failure:
#ifdef ENABLE_SCREENCOPY_DMABUF
	if (gbm_device)
		gbm_device_destroy(gbm_device);
	if (drm_fd >= 0)
		close(drm_fd);
#endif
	wayvnc_destroy(&self);
	return 1;
}
