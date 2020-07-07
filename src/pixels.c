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

#include "pixels.h"

#include <pixman.h>
#include <wayland-client.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>
#include <stdbool.h>

enum wl_shm_format fourcc_to_wl_shm(uint32_t in)
{
	assert(!(in & DRM_FORMAT_BIG_ENDIAN));

	switch (in) {
	case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
	}

	return in;
}

bool fourcc_to_pixman_fmt(pixman_format_code_t* dst, uint32_t src)
{
	assert(!(src & DRM_FORMAT_BIG_ENDIAN));

	/* TODO: Add more, perhaps with the help of
	 * https://github.com/afrantzis/pixel-format-guide
	 */
	switch (src) {
	case DRM_FORMAT_ARGB8888: *dst = PIXMAN_a8r8g8b8; break;
	case DRM_FORMAT_XRGB8888: *dst = PIXMAN_x8r8g8b8; break;
	case DRM_FORMAT_ABGR8888: *dst = PIXMAN_a8b8g8r8; break;
	case DRM_FORMAT_XBGR8888: *dst = PIXMAN_x8b8g8r8; break;
	case DRM_FORMAT_RGBA8888: *dst = PIXMAN_r8g8b8a8; break;
	case DRM_FORMAT_RGBX8888: *dst = PIXMAN_r8g8b8x8; break;
	case DRM_FORMAT_BGRA8888: *dst = PIXMAN_b8g8r8a8; break;
	case DRM_FORMAT_BGRX8888: *dst = PIXMAN_b8g8r8x8; break;
	default: return false;
	}

	return true;
}

