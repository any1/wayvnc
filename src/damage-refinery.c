/*
 * Copyright (c) 2020 - 2021 Andri Yngvason
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

#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <pixman.h>
#include <sys/param.h>

#include "damage-refinery.h"
#include "buffer.h"
#include "util.h"
#include "murmurhash.h"

#define HASH_SEED 0

int damage_refinery_init(struct damage_refinery* self, uint32_t width,
		uint32_t height)
{
	self->width = width;
	self->height = height;

	uint32_t twidth = UDIV_UP(width, 32);
	uint32_t theight = UDIV_UP(height, 32);

	self->hashes = calloc(twidth * theight, sizeof(*self->hashes));
	if (!self->hashes)
		return -1;

	return 0;
}

int damage_refinery_resize(struct damage_refinery* self, uint32_t width,
		uint32_t height)
{
	if (width == self->width && height == self->height)
		return 0;

	damage_refinery_destroy(self);
	return damage_refinery_init(self, width, height);
}

void damage_refinery_destroy(struct damage_refinery* self)
{
	free(self->hashes);
}

static uint32_t damage_hash_tile(struct damage_refinery* self, uint32_t tx,
		uint32_t ty, const struct wv_buffer* buffer)
{
	// TODO: Support different pixel sizes
	uint32_t* pixels = buffer->pixels;
	int pixel_stride = buffer->stride / 4;

	if (buffer->y_inverted) {
		pixels += (buffer->height - 1) * pixel_stride;
		pixel_stride *= -1;
	}

	int x_start = tx * 32;
	int x_stop = MIN((tx + 1) * 32, self->width);
	int y_start = ty * 32;
	int y_stop = MIN((ty + 1) * 32, self->height);

	uint32_t hash = 0;

	for (int y = y_start; y < y_stop; ++y)
		hash = murmurhash((void*)&(pixels[x_start + y * pixel_stride]),
				4 * (x_stop - x_start), hash);

	return hash;
}

static uint32_t* damage_tile_hash_ptr(struct damage_refinery* self,
		uint32_t tx, uint32_t ty)
{
	uint32_t twidth = UDIV_UP(self->width, 32);
	return &self->hashes[tx + ty * twidth];
}

static void damage_refine_tile(struct damage_refinery* self,
		struct pixman_region16* refined, uint32_t tx, uint32_t ty,
		const struct wv_buffer* buffer)
{
	uint32_t hash = damage_hash_tile(self, tx, ty, buffer);
	uint32_t* old_hash_ptr = damage_tile_hash_ptr(self, tx, ty);
	int is_damaged = hash != *old_hash_ptr;
	*old_hash_ptr = hash;

	if (is_damaged)
		pixman_region_union_rect(refined, refined, tx * 32, ty * 32, 32,
				32);
}

static void tile_region_from_region(struct pixman_region16* dst,
		struct pixman_region16* src)
{
	int n_rects = 0;
	struct pixman_box16* rects = pixman_region_rectangles(src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int x1 = rects[i].x1 / 32;
		int y1 = rects[i].y1 / 32;
		int x2 = UDIV_UP(rects[i].x2, 32);
		int y2 = UDIV_UP(rects[i].y2, 32);

		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

void damage_refine(struct damage_refinery* self,
		struct pixman_region16* refined, 
		struct pixman_region16* hint,
		const struct wv_buffer* buffer)
{
	assert(self->width == (uint32_t)buffer->width &&
	       self->height == (uint32_t)buffer->height);

	struct pixman_region16 tile_region;
	pixman_region_init(&tile_region);
	tile_region_from_region(&tile_region, hint);

	int n_rects = 0;
	struct pixman_box16* rects = pixman_region_rectangles(&tile_region,
			&n_rects);

	for (int i = 0; i < n_rects; ++i)
		for (int ty = rects[i].y1; ty < rects[i].y2; ++ty)
			for (int tx = rects[i].x1; tx < rects[i].x2; ++tx)
				damage_refine_tile(self, refined, tx, ty, buffer);

	pixman_region_fini(&tile_region);
	pixman_region_intersect_rect(refined, refined, 0, 0, self->width,
			self->height);
}
