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

#include "toplevel.h"
#include "ext-foreign-toplevel-list-v1.h"
#include "strlcpy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <neatvnc.h>

struct toplevel* toplevel_from_image_source(const struct image_source* source)
{
	assert(image_source_is_toplevel(source));
	return (struct toplevel*)source;
}

void toplevel_destroy(struct toplevel *self)
{
	wl_list_remove(&self->link);
	free(self);
}

void toplevel_image_source_describe(const struct image_source* self, char* dst,
		size_t maxlen)
{
	struct toplevel* toplevel = toplevel_from_image_source(self);
	snprintf(dst, maxlen, "Toplevel %s", toplevel->identifier);
}

struct image_source_impl image_source_impl = {
	.describe = toplevel_image_source_describe,
};

bool image_source_is_toplevel(const struct image_source* source)
{
	return source->impl == &image_source_impl;
}

static void handle_closed(void* data,
		struct ext_foreign_toplevel_handle_v1* handle)
{
	struct toplevel* self = data;
	if (self->on_closed)
		self->on_closed(self);
}

static void handle_done(void* data,
		struct ext_foreign_toplevel_handle_v1* handle)
{
	struct toplevel* self = data;
	nvnc_trace("Added toplevel: %s, app_id: %s, title: %s",
			self->identifier, self->app_id, self->title);
}

static void handle_title(void* data,
		struct ext_foreign_toplevel_handle_v1* handle,
		const char* value)
{
	struct toplevel* self = data;
	strlcpy(self->title, value, sizeof(self->title));
}

static void handle_app_id(void* data,
		struct ext_foreign_toplevel_handle_v1* handle,
		const char* value)
{
	struct toplevel* self = data;
	strlcpy(self->app_id, value, sizeof(self->app_id));
}

static void handle_identifier(void* data,
		struct ext_foreign_toplevel_handle_v1* handle,
		const char* value)
{
	struct toplevel* self = data;
	strlcpy(self->identifier, value, sizeof(self->identifier));
}

struct toplevel* toplevel_new(struct ext_foreign_toplevel_handle_v1* handle)
{
	struct toplevel* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	wl_list_init(&self->link);

	self->handle = handle;

	static struct ext_foreign_toplevel_handle_v1_listener listener = {
		.closed = handle_closed,
		.done = handle_done,
		.title = handle_title,
		.app_id = handle_app_id,
		.identifier = handle_identifier,
	};
	ext_foreign_toplevel_handle_v1_add_listener(handle, &listener, self);

	image_source_init(&self->image_source, &image_source_impl);

	return self;
}

void toplevel_list_destroy(struct wl_list* list)
{
	struct toplevel* toplevel;
	struct toplevel* tmp;
	
	wl_list_for_each_safe(toplevel, tmp, list, link)
		toplevel_destroy(toplevel);
}

struct toplevel* toplevel_find_by_identifier(struct wl_list* list,
		const char *identifier)
{
	struct toplevel* toplevel;
	wl_list_for_each(toplevel, list, link)
		if (strcmp(identifier, toplevel->identifier) == 0)
			return toplevel;
	return NULL;
}
