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

#include "image-source.h"

#include <assert.h>
#include <stdlib.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

const char* image_source_power_state_name(enum image_source_power_state state)
{
	switch(state) {
	case IMAGE_SOURCE_POWER_ON:
		return "ON";
	case IMAGE_SOURCE_POWER_OFF:
		return "OFF";
	case IMAGE_SOURCE_POWER_UNKNOWN:
		return "UNKNOWN";
	}
	abort();
	return NULL;
}

void image_source_init(struct image_source* self, struct image_source_impl* impl)
{
	self->impl = impl;
	observable_init(&self->observable.power_change);
}

void image_source_deinit(struct image_source* self)
{
	assert(self->impl);
	observable_deinit(&self->observable.power_change);
	if (self->impl->deinit)
		self->impl->deinit(self);
}

void image_source_destroy(struct image_source* self)
{
	image_source_deinit(self);
	free(self);
}

bool image_source_get_dimensions(const struct image_source* self,
		int* width, int *height)
{
	assert(self->impl);
	if (self->impl->get_dimensions) {
		self->impl->get_dimensions(self, width, height);
		return true;
	}
	return false;
}

enum wl_output_transform image_source_get_transform(
		const struct image_source* self)
{
	assert(self->impl);
	if (self->impl->get_transform)
		return self->impl->get_transform(self);
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

enum image_source_power_state image_source_get_power(
		const struct image_source* self)
{
	assert(self->impl);
	if (self->impl->get_power_state)
		return self->impl->get_power_state(self);
	return IMAGE_SOURCE_POWER_ON;
}

const char* image_source_describe(const struct image_source* self,
		char* dst, size_t maxlen)
{
	assert(self->impl && self->impl->describe);
	self->impl->describe(self, dst, maxlen);
	return dst;
}

int image_source_acquire_power_on(struct image_source* self)
{
	assert(self->impl);
	if (self->impl->acquire_power_on)
		return self->impl->acquire_power_on(self);
	return 1;
}

void image_source_release_power_on(struct image_source* self)
{
	assert(self->impl);
	if (self->impl->release_power_on)
		self->impl->release_power_on(self);
}

static bool is_transform_90_degrees(enum wl_output_transform transform)
{
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return true;
	default:
		break;
	}

	return false;
}

bool image_source_get_transformed_dimensions(const struct image_source* self,
		int* width, int* height)
{
	int w, h;
	if (!image_source_get_dimensions(self, &w, &h))
		return false;
	if (is_transform_90_degrees(image_source_get_transform(self))) {
		if (width)
			*width = h;
		if (height)
			*height = w;
	} else {
		if (width)
			*width = w;
		if (height)
			*height = h;
	}
	return true;
}
