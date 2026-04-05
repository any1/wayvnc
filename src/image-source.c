/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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
#include <math.h>

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
	observable_init(&self->observable.geometry_change);
	observable_init(&self->observable.destroyed);
}

void image_source_deinit(struct image_source* self)
{
	assert(self->impl);
	observable_notify(&self->observable.destroyed, NULL);
	observable_deinit(&self->observable.destroyed);
	observable_deinit(&self->observable.power_change);
	observable_deinit(&self->observable.geometry_change);
	if (self->impl->deinit)
		self->impl->deinit(self);
}

void image_source_destroy(struct image_source* self)
{
	if (!self)
		return;
	image_source_deinit(self);
	free(self);
}

bool image_source_get_logical_size(const struct image_source* self,
		int* width, int *height)
{
	assert(self->impl);
	if (self->impl->get_logical_size) {
		self->impl->get_logical_size(self, width, height);
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

bool image_source_get_buffer_size(const struct image_source* self,
		int* width, int* height)
{
	assert(self->impl);
	if (self->impl->get_buffer_size) {
		self->impl->get_buffer_size(self, width, height);
		return true;
	}
	return false;
}

bool image_source_get_scale(const struct image_source* self,
		double* h_scale, double* v_scale)
{
	int logical_w, logical_h;
	int buffer_w, buffer_h;

	if (!image_source_get_logical_size(self,
				&logical_w, &logical_h))
		return false;

	if (!image_source_get_buffer_size(self,
				&buffer_w, &buffer_h))
		return false;

	if (buffer_w == 0 || buffer_h == 0)
		return false;

	if (h_scale)
		*h_scale = (double)logical_w / buffer_w;

	if (v_scale)
		*v_scale = (double)logical_h / buffer_h;

	return true;
}

double image_source_get_min_scale(const struct image_source* self)
{
	assert(self->impl);
	if (self->impl->get_min_scale)
		return self->impl->get_min_scale(self);

	double h_scale, v_scale;
	if (!image_source_get_scale(self, &h_scale, &v_scale))
		return 1.0;

	return fmin(h_scale, v_scale);
}
