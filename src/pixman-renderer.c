#include <stdlib.h>
#include <pixman.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "buffer.h"
#include "pixels.h"
#include "transform-util.h"

void wv_pixman_render(struct nvnc_fb* dst, const struct wv_buffer* src,
		enum wl_output_transform transform,
		struct pixman_region16* damage)
{
	uint32_t* dst_pixels = nvnc_fb_get_addr(dst);
	uint32_t dst_width = nvnc_fb_get_width(dst);
	uint32_t dst_height = nvnc_fb_get_height(dst);

	// TODO: Check that both buffers have the same dimensions after applying
	// transform

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			PIXMAN_x8b8g8r8, dst_width, dst_height, dst_pixels,
			4 * dst_width);

	intptr_t src_offset = src->y_inverted ?
		src->stride * (src->height - 1) : 0;
	void* src_pixels = (void*)((intptr_t)src->pixels + src_offset);
	int src_stride = src->y_inverted ? -src->stride : src->stride;

	pixman_format_code_t src_fmt = 0;
	fourcc_to_pixman_fmt(&src_fmt, src->format);
	pixman_image_t* srcimg = pixman_image_create_bits_no_clear(
			src_fmt, src->width, src->height, src_pixels,
			src_stride);

	pixman_transform_t pxform;
	wv_pixman_transform_from_wl_output_transform(&pxform, transform,
			src->width, src->height);

	pixman_image_set_transform(srcimg, &pxform);
	pixman_image_set_clip_region(dstimg, damage);

	pixman_image_composite(PIXMAN_OP_OVER, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			dst_width, dst_height);

	pixman_image_unref(srcimg);
	pixman_image_unref(dstimg);
}
