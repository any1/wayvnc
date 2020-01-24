/*
 * Copyright (c) 2019 Andri Yngvason
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

#include <stdint.h>
#include <neatvnc.h>
#include "wlr-virtual-pointer-unstable-v1.h"

#include "output.h"

struct pointer {
	struct nvnc* vnc;
	struct zwlr_virtual_pointer_v1* pointer;

	enum nvnc_button_mask current_mask;

	uint32_t current_x;
	uint32_t current_y;

	const struct output* output;
};

int pointer_init(struct pointer* self);
void pointer_destroy(struct pointer* self);

void pointer_set(struct pointer* self, uint32_t x, uint32_t y,
		 enum nvnc_button_mask button_mask);
