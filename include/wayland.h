/*
 * Copyright (c) 2025 Andri Yngvason
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

#pragma once

#include "observer.h"

#include <stdbool.h>
// TODO: Remove this once wl_list is replaced with queue.h
#include <wayland-client-core.h>

#define X_WAYLAND_PROTOCOLS \
	X(ext_foreign_toplevel_image_capture_source_manager_v1) \
	X(ext_foreign_toplevel_list_v1) \
	X(ext_image_copy_capture_manager_v1) \
	X(ext_output_image_capture_source_manager_v1) \
	X(ext_transient_seat_manager_v1) \
	X(wl_shm) \
	X(zwlr_data_control_manager_v1) \
	X(zwlr_output_power_manager_v1) \
	X(zwlr_screencopy_manager_v1) \
	X(zwlr_virtual_pointer_manager_v1) \
	X(zwp_linux_dmabuf_v1) \
	X(zwp_virtual_keyboard_manager_v1) \
	X(zxdg_output_manager_v1)

#define X_WAYLAND_OBSERVABLES \
	X(destroyed) \
	X(output_added) \
	X(output_removed) \
	X(seat_added) \
	X(seat_removed)

struct aml_handler;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_foreign_toplevel_list_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_transient_seat_manager_v1;
struct wl_shm;
struct zwlr_data_control_manager_v1;
struct zwlr_output_power_manager_v1;
struct zwlr_screencopy_manager_v1;
struct zwlr_virtual_pointer_manager_v1;
struct zwp_linux_dmabuf_v1;
struct zwp_virtual_keyboard_manager_v1;
struct zxdg_output_manager_v1;

enum wayland_flags {
	WAYLAND_FLAG_ENABLE_INPUT = 1 << 0,
	WAYLAND_FLAG_ENABLE_TOPLEVEL_CAPTURE = 1 << 1,
	WAYLAND_FLAG_ENABLE_TRANSIENT_SEAT = 1 << 2,
};

struct wayland {
	bool is_initialising;
	enum wayland_flags flags;

	struct aml_handler* wl_handler;
	struct wl_display* display;
	struct wl_registry* registry;

	struct wl_list outputs;
	struct wl_list seats;
	struct wl_list toplevels;

#define X(x) struct x* x;
	X_WAYLAND_PROTOCOLS
#undef X

	struct {
#define X(x) struct observable x;
		X_WAYLAND_OBSERVABLES
#undef X
	} observable;
};

struct wayland* wayland_connect(const char* display, enum wayland_flags flags);
void wayland_destroy(struct wayland* self);
