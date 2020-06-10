/*
 * Copyright (c) 2020 Andri Yngvason
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>
#include <pixman.h>
#include <aml.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

struct damage_work {
	void (*on_done)(struct pixman_region16*, void*);
	void* userdata;
	uint8_t* buffer;
	uint32_t width, height;
	struct pixman_region16 damage;
};

struct damage_work* damage_work_new(void)
{
	struct damage_work* work = calloc(1, sizeof(*work));
	if (!work)
		return NULL;

	pixman_region_init(&work->damage);

	return work;
}

void damage_work_free(void* ud)
{
	struct damage_work* work = ud;
	free(work->buffer);
	pixman_region_fini(&work->damage);
	free(work);
}

bool damage_check_32_byte_block(const void* block)
{
	const uint64_t* a = block;
	return a[0] || a[1] || a[2] || a[3];
}

void damage_check_row(uint8_t* dst, const uint16_t* src, uint32_t width)
{
	uint32_t aligned_width = (width / 32) * 32;

	for (uint32_t x = 0; x < aligned_width; x += 32)
		dst[x / 32] |= damage_check_32_byte_block(&src[x])
			     | damage_check_32_byte_block(&src[x + 1]);

	for (uint32_t x = aligned_width; x < width; ++x)
		dst[x / 32] |= src[x] | src[x + 1];
}

void damage_check_tile_row(struct pixman_region16* damage,
                           uint8_t* row_buffer, const uint16_t* buffer,
			   uint32_t y_start, uint32_t width, uint32_t height)
{
	uint32_t tiled_width = UDIV_UP(width, 32);

	memset(row_buffer, 0, tiled_width);

	for (uint32_t y = y_start; y < y_start + height; ++y)
		damage_check_row(row_buffer, buffer + y * width, width);

	for (uint32_t x = 0; x < tiled_width; ++x)
		if (row_buffer[x])
			pixman_region_union_rect(damage, damage,
			                         x * 32, y_start, 32, 32);
}

void damage_check(struct pixman_region16* damage, const uint16_t* buffer,
                  uint32_t width, uint32_t height, struct pixman_box16* hint)
{
	uint32_t tiled_width = UDIV_UP(width, 32);
	uint8_t* row_buffer = malloc(tiled_width);
	assert(row_buffer);

	for (uint32_t y = 0; y < height; y += 32) {
		uint32_t current_height = MIN(32, height - y);
		damage_check_tile_row(damage, row_buffer, buffer, y, width,
		                      current_height);
	}

	pixman_region_intersect_rect(damage, damage, 0, 0, width, height);

	free(row_buffer);
}

static void do_damage_check(void* aml_obj)
{
	struct damage_work* priv = aml_get_userdata(aml_obj);
	damage_check(&priv->damage, priv->buffer, priv->width, priv->height, NULL);
}

static void on_damage_check_done(void* aml_obj)
{
	struct damage_work* priv = aml_get_userdata(aml_obj);
	priv->on_done(&priv->damage, priv->userdata);
}

// TODO: Use the hint
// TODO: Split rows between jobs
// XXX: This takes ownership of the damage buffer
int damage_check_async(uint8_t* buffer, uint32_t width, uint32_t height,
                       struct pixman_box16* hint,
                       void (*on_done)(struct pixman_region16*, void*),
                       void* userdata)
{
	struct damage_work* priv = damage_work_new();
	if (!priv)
		return -1;

	priv->buffer = buffer;
	priv->width = width;
	priv->height = height;
	priv->on_done = on_done;
	priv->userdata = userdata;

	struct aml_work* work;
	work = aml_work_new(do_damage_check, on_damage_check_done, priv,
	                    damage_work_free);
	if (!work)
		goto oom;

	int rc = aml_start(aml_get_default(), work);
	aml_unref(work);

	return rc;

oom:
	damage_work_free(priv);
	return -1;
}

void damage_dump(FILE* stream, struct pixman_region16* damage,
                 uint32_t width, uint32_t height, uint32_t tile_size)
{
	uint32_t tiled_width = UDIV_UP(width, tile_size);
	uint32_t tiled_height = UDIV_UP(height, tile_size);

	fprintf(stream, "\033[2J");

	for (uint32_t y = 0; y < tiled_height; ++y) {
		for (uint32_t x = 0; x < tiled_width; ++x) {
			struct pixman_box16 box = {
				.x1 = x * tile_size,
				.x2 = ((x + 1) * tile_size) - 1,
				.y1 = y * tile_size,
				.y2 = ((y + 1) * tile_size) - 1,
			};

			pixman_region_overlap_t overlap =
				pixman_region_contains_rectangle(damage, &box); 

			putc(overlap == PIXMAN_REGION_IN ? 'x' : ' ', stream);
		}
		putc('\n', stream);
	}
}
