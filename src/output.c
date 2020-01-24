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

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <wayland-client.h>

#include "output.h"
#include "strlcpy.h"
#include "logging.h"

#include "xdg-output-unstable-v1.h"

static void output_handle_geometry(void* data, struct wl_output* wl_output,
				   int32_t x, int32_t y, int32_t phys_width,
				   int32_t phys_height, int32_t subpixel,
				   const char* make, const char* model,
				   int32_t transform)
{
	struct output* output = data;

	strlcpy(output->make, make, sizeof(output->make));
	strlcpy(output->model, model, sizeof(output->model));
}

static void output_handle_mode(void* data, struct wl_output* wl_output,
			       uint32_t flags, int32_t width, int32_t height,
			       int32_t refresh)
{
}

static void output_handle_done(void* data, struct wl_output* wl_output)
{
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
	zxdg_output_v1_destroy(output->xdg_output);
	wl_output_destroy(output->wl_output);
	free(output);
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

struct output* output_new(struct wl_output* wl_output, uint32_t id)
{
	struct output* output = calloc(1, sizeof(*output));
	if (!output) {
		log_error("OOM\n");
		return NULL;
	}

	output->wl_output = wl_output;
	output->id = id;

	wl_output_add_listener(output->wl_output, &output_listener,
			output);

	return output;
}

void output_logical_position(void* data, struct zxdg_output_v1* xdg_output,
                             int32_t x, int32_t y)
{
	struct output* self = data;

	self->x = x;
	self->y = y;
}

void output_logical_size(void* data, struct zxdg_output_v1* xdg_output,
                         int32_t width, int32_t height)
{
	struct output* self = data;

	self->width = width;
	self->height = height;
}

void output_name(void* data, struct zxdg_output_v1* xdg_output,
                 const char* name)
{
	struct output* self = data;

	strlcpy(self->name, name, sizeof(self->name));
}

void output_description(void* data, struct zxdg_output_v1* xdg_output,
                        const char* description)
{
	struct output* self = data;

	strlcpy(self->description, description, sizeof(self->description));
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = NULL, /* Deprecated */
	.name = output_name,
	.description = output_description,
};

void output_set_xdg_output(struct output* self,
                           struct zxdg_output_v1* xdg_output)
{
	self->xdg_output = xdg_output;

	zxdg_output_v1_add_listener(self->xdg_output, &xdg_output_listener,
	                            self);
}

struct output* output_find_by_id(struct wl_list* list, uint32_t id)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		if (output->id == id)
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
