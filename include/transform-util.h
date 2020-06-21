#pragma once

#include <wayland-client.h>
#include <pixman.h>

void wv_region_transform(struct pixman_region16 *dst,
		struct pixman_region16 *src, enum wl_output_transform transform,
		int width, int height);

void wv_pixman_transform_from_wl_output_transform(pixman_transform_t* dst,
		enum wl_output_transform src, int width, int height);

enum wl_output_transform wv_output_transform_invert(enum wl_output_transform tr);
enum wl_output_transform wv_output_transform_compose(
		enum wl_output_transform tr_a, enum wl_output_transform tr_b);
