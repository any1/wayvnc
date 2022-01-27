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

uint32_t fourcc_from_wl_shm(enum wl_shm_format in)
{
	switch (in) {
	case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
	default:;
	}

	return in;
}
