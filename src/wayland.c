/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
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

#include "output-management.h"
#include "output.h"
#include "seat.h"
#include "toplevel.h"
#include "wayland.h"

#include <aml.h>
#include <errno.h>
#include <neatvnc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <wayland-client-protocol.h>

#include "ext-foreign-toplevel-list-v1.h"
#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"
#include "ext-data-control-v1.h"
#include "ext-transient-seat-v1.h"
#include "linux-dmabuf-unstable-v1.h"
#include "virtual-keyboard-unstable-v1.h"
#include "wlr-data-control-unstable-v1.h"
#include "wlr-output-management-unstable-v1.h"
#include "wlr-output-power-management-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "xdg-output-unstable-v1.h"

#define MAYBE_UNUSED __attribute__((unused))

#define CHECK_BIND(ext, v) ({ \
	bool is_match = strcmp(interface, ext ## _interface.name) == 0; \
	if (is_match) \
		self->ext = wl_registry_bind(registry, id, \
				&ext ## _interface, v); \
	is_match; \
})

static inline bool is_flag_set(const struct wayland* self,
		enum wayland_flags flag)
{
	return self->flags & flag;
}

static bool registry_add_input(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface, uint32_t version)
{
	struct wayland* self = data;

	if (!is_flag_set(self, WAYLAND_FLAG_ENABLE_INPUT))
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

		observable_notify(&self->observable.seat_added, seat);
		return true;
	}

	if (CHECK_BIND(zwlr_virtual_pointer_manager_v1, MIN(2, version)))
		return true;

	if (CHECK_BIND(zwp_virtual_keyboard_manager_v1, 1))
		return true;

	if (CHECK_BIND(ext_data_control_manager_v1, 1))
		return true;

	if (CHECK_BIND(zwlr_data_control_manager_v1, 2))
		return true;

	if (CHECK_BIND(ext_transient_seat_manager_v1, 1))
		return true;

	return false;
}

static void handle_toplevel_handle(void* data,
		struct ext_foreign_toplevel_list_v1* list,
		struct ext_foreign_toplevel_handle_v1* handle)
{
	struct wayland* self = data;

	struct toplevel *toplevel = toplevel_new(handle);
	wl_list_insert(&self->toplevels, &toplevel->link);
}

static void handle_toplevel_list_finished(void* data,
		struct ext_foreign_toplevel_list_v1* list)
{
	// noop
}

static bool registry_add_toplevel(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface, uint32_t version)
{
	struct wayland* self = data;

	if (!is_flag_set(self, WAYLAND_FLAG_ENABLE_TOPLEVEL_CAPTURE))
		return false;

	if (CHECK_BIND(ext_foreign_toplevel_image_capture_source_manager_v1,
				MIN(1, version)))
		return true;

	if (CHECK_BIND(ext_foreign_toplevel_list_v1, 1)) {
		static struct ext_foreign_toplevel_list_v1_listener listener = {
			.toplevel = handle_toplevel_handle,
			.finished = handle_toplevel_list_finished,
		};
		ext_foreign_toplevel_list_v1_add_listener(
				self->ext_foreign_toplevel_list_v1, &listener,
				self);

		ext_foreign_toplevel_list_v1_stop(
				self->ext_foreign_toplevel_list_v1);
		return true;
	}

	return false;
}

static void registry_add(void* data, struct wl_registry* registry,
		uint32_t id, const char* interface, uint32_t version)
{
	struct wayland* self = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		nvnc_trace("Registering new output %u", id);
		struct wl_output* wl_output =
			wl_registry_bind(registry, id, &wl_output_interface, 3);
		if (!wl_output)
			return;

		struct output* output = output_new(self, wl_output, id);
		if (!output)
			return;

		wl_list_insert(&self->outputs, &output->link);
		if (!self->is_initialising) {
			wl_display_dispatch(self->display);
			wl_display_roundtrip(self->display);

			observable_notify(&self->observable.output_added, output);
		}

		return;
	}

	if (CHECK_BIND(zxdg_output_manager_v1, 3)) {
		nvnc_trace("Registering new xdg_output_manager");
		output_setup_xdg_output_managers(self, &self->outputs);
		return;
	}

	if (CHECK_BIND(zwlr_output_power_manager_v1, 1)) {
		nvnc_trace("Registering new wlr_output_power_manager");
		return;
	}

	if (CHECK_BIND(zwlr_screencopy_manager_v1, MIN(3, version)))
		return;

	if (CHECK_BIND(ext_image_copy_capture_manager_v1, MIN(1, version)))
		return;

	if (CHECK_BIND(ext_output_image_capture_source_manager_v1,
				MIN(1, version)))
		return;

	if (CHECK_BIND(wl_shm, 1))
		return;

	if (CHECK_BIND(zwp_linux_dmabuf_v1, 3))
		return;

	// TODO: Move output manager global into this
	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		nvnc_trace("Registering new wlr_output_manager");
		struct zwlr_output_manager_v1* wlr_output_manager =
			wl_registry_bind(registry, id,
				&zwlr_output_manager_v1_interface, 1);

		// TODO: This should not be a global
		wlr_output_manager_setup(wlr_output_manager);
		return;
	}

	if (registry_add_input(data, registry, id, interface, version))
		return;

	if (registry_add_toplevel(data, registry, id, interface, version))
		return;
}

static void registry_remove(void* data, struct wl_registry* registry,
		uint32_t id)
{
	struct wayland* self = data;

	struct output* out = output_find_by_id(&self->outputs, id);
	if (out) {
		wl_list_remove(&out->link);
		observable_notify(&self->observable.output_removed, out);
		output_destroy(out);
		return;
	}

	struct seat* seat = seat_find_by_id(&self->seats, id);
	if (seat) {
		nvnc_log(NVNC_LOG_INFO, "Seat %s went away", seat->name);
		wl_list_remove(&seat->link);
		observable_notify(&self->observable.seat_removed, seat);
		seat_destroy(seat);
		return;
	}
}

static void on_wayland_event(struct aml_handler* handler)
{
	struct wayland* self = aml_get_userdata(handler);

	int rc MAYBE_UNUSED = wl_display_prepare_read(self->display);
	assert(rc == 0);

	if (wl_display_read_events(self->display) < 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			nvnc_log(NVNC_LOG_DEBUG, "Compositor has gone away.");
			wayland_destroy(self);
			return;
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to read wayland events: %m");
		}
	}

	if (wl_display_dispatch_pending(self->display) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to dispatch pending");
		wayland_destroy(self);
	}
}

struct wayland* wayland_connect(const char* display, enum wayland_flags flags)
{
	struct wayland* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->is_initialising = true;
	self->flags = flags;

	wl_list_init(&self->outputs);
	wl_list_init(&self->toplevels);
	wl_list_init(&self->seats);

#define X(x) observable_init(&self->observable.x);
	X_WAYLAND_OBSERVABLES
#undef X

	self->display = wl_display_connect(display);
	if (!self->display) {
		const char* display_name = display ? display:
			getenv("WAYLAND_DISPLAY");
		if (!display_name) {
			nvnc_log(NVNC_LOG_ERROR,"WAYLAND_DISPLAY is not set in the environment");
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Failed to connect to WAYLAND_DISPLAY=\"%s\"", display_name);
			nvnc_log(NVNC_LOG_ERROR, "Ensure wayland is running with that display name");
		}
		goto connect_failure;
	}

	self->registry = wl_display_get_registry(self->display);
	if (!self->registry) {
		nvnc_log(NVNC_LOG_ERROR, "Could not locate the wayland compositor object registry");
		goto registry_failure;
	}

	static const struct wl_registry_listener registry_listener = {
		.global = registry_add,
		.global_remove = registry_remove,
	};

	wl_registry_add_listener(self->registry, &registry_listener, self);

	wl_display_dispatch(self->display);
	wl_display_roundtrip(self->display);

	self->wl_handler = aml_handler_new(wl_display_get_fd(self->display),
	                             on_wayland_event, self, NULL);
	if (!self->wl_handler)
		goto handler_failure;

	int rc = aml_start(aml_get_default(), self->wl_handler);
	if (rc < 0)
		goto handler_failure;

	self->is_initialising = false;
	return self;

handler_failure:
	if (self->wl_handler)
		aml_unref(self->wl_handler);
	self->wl_handler = NULL;
registry_failure:
	wl_display_disconnect(self->display);
	self->display = NULL;
connect_failure:
	free(self);
	return NULL;
}

void wayland_destroy(struct wayland* self)
{
	if (!self)
		return;

	observable_notify(&self->observable.destroyed, self);

#define X(x) observable_deinit(&self->observable.x);
	X_WAYLAND_OBSERVABLES
#undef X

	// TODO: Remove once output manager is no longer a global
	wlr_output_manager_destroy();

	aml_stop(aml_get_default(), self->wl_handler);
	aml_unref(self->wl_handler);
	self->wl_handler = NULL;

	output_list_destroy(&self->outputs);
	toplevel_list_destroy(&self->toplevels);
	seat_list_destroy(&self->seats);

#define X(x) \
	if (self->x) \
		x ## _destroy(self->x); \
	self->x = NULL;

	X_WAYLAND_PROTOCOLS
#undef X

	wl_registry_destroy(self->registry);
	self->registry = NULL;

	wl_display_disconnect(self->display);
	self->display = NULL;

	free(self);
}
