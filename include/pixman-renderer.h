#pragma once

#include <wayland-client.h>

struct nvnc_fb;
struct wv_buffer;
struct pixman_region16;

void wv_pixman_render(struct nvnc_fb* dst, const struct wv_buffer* src,
		enum wl_output_transform transform,
		struct pixman_region16* damage);
