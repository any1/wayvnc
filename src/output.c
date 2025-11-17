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

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "output.h"
#include "strlcpy.h"

#include "xdg-output-unstable-v1.h"
#include "wlr-output-power-management-unstable-v1.h"

extern struct zxdg_output_manager_v1* xdg_output_manager;
extern struct zwlr_output_power_manager_v1* wlr_output_power_manager;

struct output* output_from_image_source(const struct image_source* source)
{
	assert(image_source_is_output(source));
	return (struct output*)source;
}

static void output_handle_geometry(void* data, struct wl_output* wl_output,
				   int32_t x, int32_t y, int32_t phys_width,
				   int32_t phys_height, int32_t subpixel,
				   const char* make, const char* model,
				   int32_t transform)
{
	struct output* output = data;

	output->transform = transform;

	strlcpy(output->make, make, sizeof(output->make));
	strlcpy(output->model, model, sizeof(output->model));
}

static void output_handle_mode(void* data, struct wl_output* wl_output,
			       uint32_t flags, int32_t width, int32_t height,
			       int32_t refresh)
{
	struct output* output = data;

	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;

	output->width = width;
	output->height = height;
}

static void output_handle_done(void* data, struct wl_output* wl_output)
{
	 // noop
}

static void output_handle_scale(void* data, struct wl_output* wl_output,
				int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
};

void output_destroy(struct output* output)
{
	image_source_destroy(&output->image_source);
}

void output_list_destroy(struct wl_list* list)
{
	struct output* output;
	struct output* tmp;

	wl_list_for_each_safe(output, tmp, list, link) {
		wl_list_remove(&output->link);
		output_destroy(output);
	}
}

void output_logical_position(void* data, struct zxdg_output_v1* xdg_output,
                             int32_t x, int32_t y)
{
	struct output* output = data;
	output->x = x;
	output->y = y;
	nvnc_log(NVNC_LOG_DEBUG, "output geometry: %d, %d", x, y);
}

void output_logical_size(void* data, struct zxdg_output_v1* xdg_output,
                         int32_t width, int32_t height)
{
}

void output_name(void* data, struct zxdg_output_v1* xdg_output,
                 const char* name)
{
	struct output* self = data;

	strlcpy(self->name, name, sizeof(self->name));
	self->is_headless =
		(strncmp(name, "HEADLESS-", strlen("HEADLESS-")) == 0) ||
		(strncmp(name, "NOOP-", strlen("NOOP-")) == 0);

	nvnc_trace("Output %u name: %s, headless: %s", self->id, self->name,
		self->is_headless ? "yes" : "no");
}

void output_description(void* data, struct zxdg_output_v1* xdg_output,
                        const char* description)
{
	struct output* self = data;

	strlcpy(self->description, description, sizeof(self->description));
	nvnc_trace("Output %u description: %s", self->id, self->description);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = NULL, /* Deprecated */
	.name = output_name,
	.description = output_description,
};

static void output_setup_xdg_output_manager(struct output* self)
{
	if (!xdg_output_manager || self->xdg_output)
		return;

	struct zxdg_output_v1* xdg_output =
		zxdg_output_manager_v1_get_xdg_output(
			xdg_output_manager, self->wl_output);
	self->xdg_output = xdg_output;
	zxdg_output_v1_add_listener(self->xdg_output, &xdg_output_listener,
			self);
}

static void output_power_mode(void *data,
		     struct zwlr_output_power_v1 *zwlr_output_power_v1,
		     uint32_t mode)
{
	struct output* self = data;
	nvnc_trace("Output %s power state changed to %s", self->name,
			(mode == ZWLR_OUTPUT_POWER_V1_MODE_ON) ? "ON" : "OFF");

	enum image_source_power_state old = self->power;
	switch (mode) {
	case ZWLR_OUTPUT_POWER_V1_MODE_OFF:
		self->power = IMAGE_SOURCE_POWER_OFF;
		break;
	case ZWLR_OUTPUT_POWER_V1_MODE_ON:
		self->power = IMAGE_SOURCE_POWER_ON;
		break;
	}
	if (old != self->power)
		observable_notify(&self->image_source.observable.power_change,
				NULL);
}

static void output_power_failed(void *data,
		     struct zwlr_output_power_v1 *zwlr_output_power_v1)
{
	struct output* self = data;
	nvnc_log(NVNC_LOG_WARNING, "Output %s power state failure", self->name);
	self->power = IMAGE_SOURCE_POWER_UNKNOWN;
	zwlr_output_power_v1_destroy(self->wlr_output_power);
	self->wlr_output_power = NULL;
}

static const struct zwlr_output_power_v1_listener wlr_output_power_listener = {
	.mode = output_power_mode,
	.failed = output_power_failed,
};

int output_acquire_power_on(struct output* output)
{
	if (output->wlr_output_power)
		return 1;

	if (!wlr_output_power_manager)
		return -1;

	struct zwlr_output_power_v1* wlr_output_power =
		zwlr_output_power_manager_v1_get_output_power(
				wlr_output_power_manager, output->wl_output);
	output->wlr_output_power = wlr_output_power;

	zwlr_output_power_v1_add_listener(output->wlr_output_power,
			&wlr_output_power_listener, output);

	zwlr_output_power_v1_set_mode(output->wlr_output_power,
		ZWLR_OUTPUT_POWER_V1_MODE_ON);
	return 0;
}

void output_release_power_on(struct output* output)
{
	if (!output->wlr_output_power)
		return;

	zwlr_output_power_v1_destroy(output->wlr_output_power);
	output->wlr_output_power = NULL;
	output->power = IMAGE_SOURCE_POWER_UNKNOWN;
}

struct output* output_find_by_id(struct wl_list* list, uint32_t id)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		if (output->id == id)
			return output;

	return NULL;
}

struct output* output_find_by_name(struct wl_list* list, const char* name)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		if (strcmp(output->name, name) == 0)
			return output;

	return NULL;
}

struct output* output_first(struct wl_list* list)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		return output;

	return output;
}

struct output* output_cycle(const struct wl_list* list,
		const struct output* current,
		enum output_cycle_direction direction)
{
	const struct wl_list* iter = current ? &current->link : list;
	iter = (direction == OUTPUT_CYCLE_FORWARD) ?
		iter->next : iter->prev;
	if (iter == list) {
		if (wl_list_empty(list))
			return NULL;
		iter = (direction == OUTPUT_CYCLE_FORWARD) ?
			iter->next : iter->prev;
	}
	struct output* output;
	return wl_container_of(iter, output, link);
}

void output_setup_xdg_output_managers(struct wl_list* list)
{
	struct output* output;
	wl_list_for_each(output, list, link) {
		output_setup_xdg_output_manager(output);
	}
}

static void output_image_source_get_dimensions(const struct image_source* self,
		int* width, int* height)
{
	struct output* output = output_from_image_source(self);
	if (width)
		*width = output->width;
	if (height)
		*height = output->height;
}

static enum wl_output_transform output_image_source_get_transform(
		const struct image_source* self)
{
	return output_from_image_source(self)->transform;
}

static enum image_source_power_state output_image_source_get_power_state(
		const struct image_source* self)
{
	return output_from_image_source(self)->power;
}

static void output_image_source_describe(const struct image_source* self,
		char* dst, size_t maxlen)
{
	struct output* output = output_from_image_source(self);
	snprintf(dst, maxlen, "output %s", output->name);
}

static int output_image_source_acquire_power_on(struct image_source* self)
{
	return output_acquire_power_on(output_from_image_source(self));
}

static void output_image_source_release_power_on(struct image_source* self)
{
	output_release_power_on(output_from_image_source(self));
}

static void output_image_source_deinit(struct image_source* base)
{
	struct output* output = output_from_image_source(base);
	output_release_power_on(output);
	if (output->xdg_output)
		zxdg_output_v1_destroy(output->xdg_output);
	if (output->wlr_output_power)
		zwlr_output_power_v1_destroy(output->wlr_output_power);
	wl_output_destroy(output->wl_output);
}

static struct image_source_impl image_source_impl = {
	.get_dimensions = output_image_source_get_dimensions,
	.get_transform = output_image_source_get_transform,
	.get_power_state = output_image_source_get_power_state,
	.describe = output_image_source_describe,
	.acquire_power_on = output_image_source_acquire_power_on,
	.release_power_on = output_image_source_release_power_on,
	.deinit = output_image_source_deinit,
};

bool image_source_is_output(const struct image_source* self)
{
	return self->impl == &image_source_impl;
}

struct output* output_new(struct wl_output* wl_output, uint32_t id)
{
	struct output* output = calloc(1, sizeof(*output));
	if (!output) {
		nvnc_log(NVNC_LOG_ERROR, "OOM");
		return NULL;
	}

	image_source_init(&output->image_source, &image_source_impl);

	output->wl_output = wl_output;
	output->id = id;
	output->power = IMAGE_SOURCE_POWER_UNKNOWN;

	wl_output_add_listener(output->wl_output, &output_listener,
			output);

	output_setup_xdg_output_manager(output);

	return output;
}
