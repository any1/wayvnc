#include <stdlib.h>
#include <pixman.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "buffer.h"
#include "pixels.h"

void pixman_transform_from_wl_output_transform(pixman_transform_t* dst,
		enum wl_output_transform src, int width, int height)
{
#define F1 pixman_fixed_1
	switch (src) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_180:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	}
#undef F1

	abort();
}

/* Borrowed these from wlroots */
enum wl_output_transform wv_output_transform_invert(
		enum wl_output_transform tr) {
	if ((tr & WL_OUTPUT_TRANSFORM_90) && !(tr & WL_OUTPUT_TRANSFORM_FLIPPED)) {
		tr ^= WL_OUTPUT_TRANSFORM_180;
	}
	return tr;
}

enum wl_output_transform wv_output_transform_compose(
		enum wl_output_transform tr_a, enum wl_output_transform tr_b) {
	uint32_t flipped = (tr_a ^ tr_b) & WL_OUTPUT_TRANSFORM_FLIPPED;
	uint32_t rotation_mask = WL_OUTPUT_TRANSFORM_90 | WL_OUTPUT_TRANSFORM_180;
	uint32_t rotated;
	if (tr_b & WL_OUTPUT_TRANSFORM_FLIPPED) {
		// When a rotation of k degrees is followed by a flip, the
		// equivalent transform is a flip followed by a rotation of
		// -k degrees.
		rotated = (tr_b - tr_a) & rotation_mask;
	} else {
		rotated = (tr_a + tr_b) & rotation_mask;
	}
	return flipped | rotated;
}

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
	pixman_transform_from_wl_output_transform(&pxform, transform, width,
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
