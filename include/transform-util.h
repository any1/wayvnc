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
