#include <stdlib.h>
#include <wayland-client.h>
#include <pixman.h>

/* Note: This function yields the inverse pixman transform of the
 * wl_output_transform.
 */
void wv_pixman_transform_from_wl_output_transform(pixman_transform_t* dst,
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
void wv_region_transform(struct pixman_region16* dst,
		struct pixman_region16* src, enum wl_output_transform transform,
		int width, int height)
{
	if (transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		pixman_region_copy(dst, src);
		return;
	}

	int nrects = 0;
	pixman_box16_t* src_rects = pixman_region_rectangles(src, &nrects);

	pixman_box16_t* dst_rects = malloc(nrects * sizeof(*dst_rects));
	if (dst_rects == NULL) {
		return;
	}

	for (int i = 0; i < nrects; ++i) {
		switch (transform) {
		case WL_OUTPUT_TRANSFORM_NORMAL:
			dst_rects[i].x1 = src_rects[i].x1;
			dst_rects[i].y1 = src_rects[i].y1;
			dst_rects[i].x2 = src_rects[i].x2;
			dst_rects[i].y2 = src_rects[i].y2;
			break;
		case WL_OUTPUT_TRANSFORM_90:
			dst_rects[i].x1 = height - src_rects[i].y2;
			dst_rects[i].y1 = src_rects[i].x1;
			dst_rects[i].x2 = height - src_rects[i].y1;
			dst_rects[i].y2 = src_rects[i].x2;
			break;
		case WL_OUTPUT_TRANSFORM_180:
			dst_rects[i].x1 = width - src_rects[i].x2;
			dst_rects[i].y1 = height - src_rects[i].y2;
			dst_rects[i].x2 = width - src_rects[i].x1;
			dst_rects[i].y2 = height - src_rects[i].y1;
			break;
		case WL_OUTPUT_TRANSFORM_270:
			dst_rects[i].x1 = src_rects[i].y1;
			dst_rects[i].y1 = width - src_rects[i].x2;
			dst_rects[i].x2 = src_rects[i].y2;
			dst_rects[i].y2 = width - src_rects[i].x1;
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED:
			dst_rects[i].x1 = width - src_rects[i].x2;
			dst_rects[i].y1 = src_rects[i].y1;
			dst_rects[i].x2 = width - src_rects[i].x1;
			dst_rects[i].y2 = src_rects[i].y2;
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			dst_rects[i].x1 = src_rects[i].y1;
			dst_rects[i].y1 = src_rects[i].x1;
			dst_rects[i].x2 = src_rects[i].y2;
			dst_rects[i].y2 = src_rects[i].x2;
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
			dst_rects[i].x1 = src_rects[i].x1;
			dst_rects[i].y1 = height - src_rects[i].y2;
			dst_rects[i].x2 = src_rects[i].x2;
			dst_rects[i].y2 = height - src_rects[i].y1;
			break;
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			dst_rects[i].x1 = height - src_rects[i].y2;
			dst_rects[i].y1 = width - src_rects[i].x2;
			dst_rects[i].x2 = height - src_rects[i].y1;
			dst_rects[i].y2 = width - src_rects[i].x1;
			break;
		}
	}

	pixman_region_fini(dst);
	pixman_region_init_rects(dst, dst_rects, nrects);
	free(dst_rects);
}

enum wl_output_transform wv_output_transform_invert(enum wl_output_transform tr)
{
	if ((tr & WL_OUTPUT_TRANSFORM_90) && !(tr & WL_OUTPUT_TRANSFORM_FLIPPED)) {
		tr ^= WL_OUTPUT_TRANSFORM_180;
	}
	return tr;
}

enum wl_output_transform wv_output_transform_compose(
		enum wl_output_transform tr_a, enum wl_output_transform tr_b)
{
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
