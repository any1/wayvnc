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

#define LOWER_R r
#define LOWER_G g
#define LOWER_B b
#define LOWER_A a
#define LOWER_X x
#define LOWER_
#define LOWER(x) LOWER_##x

#define CONCAT_(a, b) a ## b
#define CONCAT(a, b) CONCAT_(a, b)

#define FMT_DRM(x, y, z, v, a, b, c, d) DRM_FORMAT_##x##y##z##v##a##b##c##d

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(x)), a), LOWER(y)), b), LOWER(z)), c), LOWER(v)), d)
#else
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(v)), d), LOWER(z)), c), LOWER(y)), b), LOWER(x)), a)
#endif

	switch (src) {
#define X(...) \
	case FMT_DRM(__VA_ARGS__): *dst = FMT_PIXMAN(__VA_ARGS__); break

	/* 32 bits */
	X(A,R,G,B,8,8,8,8);
	X(A,B,G,R,8,8,8,8);
	X(X,R,G,B,8,8,8,8);
	X(X,B,G,R,8,8,8,8);
	X(R,G,B,A,8,8,8,8);
	X(B,G,R,A,8,8,8,8);
	X(R,G,B,X,8,8,8,8);
	X(B,G,R,X,8,8,8,8);

	/* 24 bits */
	X(R,G,B,,8,8,8,);
	X(B,G,R,,8,8,8,);

	/* 16 bits */
	X(R,G,B,,5,6,5,);
	X(B,G,R,,5,6,5,);

	/* These are incompatible on big endian */
#if __BYTE_ORDER__ == __LITTLE_ENDIAN__
	X(A,R,G,B,1,5,5,5);
	X(A,B,G,R,1,5,5,5);
	X(X,R,G,B,1,5,5,5);
	X(X,B,G,R,1,5,5,5);
	X(A,R,G,B,4,4,4,4);
	X(A,B,G,R,4,4,4,4);
	X(X,R,G,B,4,4,4,4);
	X(X,B,G,R,4,4,4,4);
#endif

#undef X

	default: return false;
	}

	return true;
}

