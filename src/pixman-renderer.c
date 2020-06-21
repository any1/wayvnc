#include <stdlib.h>
#include <pixman.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "buffer.h"
#include "pixels.h"
#include "transform-util.h"

void wv_pixman_render(struct nvnc_fb* dst, const struct wv_buffer* src,
		enum wl_output_transform transform,
		struct pixman_region16* damage) {
	uint32_t* addr = nvnc_fb_get_addr(dst);
	uint32_t width = nvnc_fb_get_width(dst);
	uint32_t height = nvnc_fb_get_height(dst);

	// TODO: Check that both buffers have the same dimensions

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			PIXMAN_x8b8g8r8, width, height, addr, 4 * width);

	pixman_format_code_t src_fmt = 0;
	fourcc_to_pixman_fmt(&src_fmt, src->format);

	pixman_image_t* srcimg = pixman_image_create_bits_no_clear(
			src_fmt, width, height, src->pixels, src->stride);

	if (src->y_inverted)
		transform = wv_output_transform_compose(
				WL_OUTPUT_TRANSFORM_FLIPPED_180, transform);

	pixman_transform_t pxform;
	wv_pixman_transform_from_wl_output_transform(&pxform, transform, width,
			height);

	pixman_image_set_transform(srcimg, &pxform);
	pixman_image_set_clip_region(dstimg, damage);

	pixman_image_composite(PIXMAN_OP_OVER, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			width, height);

	pixman_image_unref(srcimg);
	pixman_image_unref(dstimg);
}
