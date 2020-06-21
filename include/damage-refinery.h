#pragma once

#include <stdint.h>

struct pixman_region16;
struct wv_buffer;

struct damage_refinery {
	uint32_t* hashes;
	uint32_t width;
	uint32_t height;
};

int damage_refinery_init(struct damage_refinery* self, uint32_t width,
		uint32_t height);
void damage_refinery_destroy(struct damage_refinery* self);

void damage_refine(struct damage_refinery* self,
		struct pixman_region16* refined, 
		struct pixman_region16* hint,
		const struct wv_buffer* buffer);
