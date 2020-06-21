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

#define HASH_SEED 0xdeadbeef

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

void damage_refinery_destroy(struct damage_refinery* self)
{
	free(self->hashes);
}

static size_t damage_copy_tile(struct damage_refinery* self, void* tile_buffer,
		uint32_t tx, uint32_t ty, const struct wv_buffer* buffer)
{
	// TODO: Support different pixel sizes
	uint32_t* tile = tile_buffer;
	uint32_t* pixels = buffer->pixels;
	int pixel_stride = buffer->stride / 4;

	size_t i = 0;
	int x_start = tx * 32;
	int x_stop = MIN(((int)tx + 1) * 32, buffer->width);
	int y_start = ty * 32;
	int y_stop = MIN(((int)ty + 1) * 32, buffer->height);

	for (int y = y_start; y < y_stop; ++y)
		for (int x = x_start; x < x_stop; ++x)
			tile[i++] = pixels[x + y * pixel_stride];

	return i;
}

static uint32_t* damage_tile_hash_ptr(struct damage_refinery* self,
		uint32_t tx, uint32_t ty)
{
	uint32_t twidth = UDIV_UP(self->width, 32);
	return &self->hashes[tx + ty * twidth];
}

static void damage_tile(struct damage_refinery* self,
		struct pixman_region16* refined, uint32_t tx, uint32_t ty,
		int invert_y)
{
	uint32_t theight = UDIV_UP(self->height, 32);
	if (invert_y)
		pixman_region_union_rect(refined, refined, tx * 32,
				(theight - ty - 1) * 32, 32, 32);
	else
		pixman_region_union_rect(refined, refined,
				tx * 32, ty * 32, 32, 32);
}

static void damage_refine_tile(struct damage_refinery* self,
		struct pixman_region16* refined, uint32_t tx, uint32_t ty,
		const struct wv_buffer* buffer)
{
	uint32_t tile[32 * 32];
	size_t len = damage_copy_tile(self, tile, tx, ty, buffer);
	uint32_t hash = murmurhash((void*)tile, len, HASH_SEED);
	uint32_t* old_hash_ptr = damage_tile_hash_ptr(self, tx, ty);
	int is_damaged = hash != *old_hash_ptr;
	*old_hash_ptr = hash;

	if (is_damaged)
		damage_tile(self, refined, tx, ty, buffer->y_inverted);
}

void damage_refine(struct damage_refinery* self,
		struct pixman_region16* refined, 
		struct pixman_region16* hint,
		const struct wv_buffer* buffer)
{
	// TODO: Use hint

	assert(self->width == (uint32_t)buffer->width &&
	       self->height == (uint32_t)buffer->height);

	uint32_t twidth = UDIV_UP(self->width, 32);
	uint32_t theight = UDIV_UP(self->height, 32);

	for (uint32_t ty = 0; ty < theight; ++ty)
		for (uint32_t tx = 0; tx < twidth; ++tx)
			damage_refine_tile(self, refined, tx, ty, buffer);

	pixman_region_intersect_rect(refined, refined, 0, 0, self->width,
			self->height);
}
