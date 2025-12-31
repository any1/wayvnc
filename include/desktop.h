/*
 * Copyright (c) 2025 Andri Yngvason
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

#include "image-source.h"
#include "observer.h"
#include "screencopy-interface.h"

#include <stdbool.h>

struct desktop_output;
struct desktop_capture;

struct desktop_output {
	LIST_ENTRY(desktop_output) link;
	struct output* output;
	struct desktop* desktop;
	struct observer power_change_observer;
	struct observer geometry_change_observer;
	struct screencopy* sc;
};

LIST_HEAD(desktop_output_list, desktop_output);

struct desktop {
	struct image_source image_source;
	struct desktop_output_list outputs;

	struct observer output_added_observer;
	struct observer output_removed_observer;

	struct desktop_capture* capture;
};

struct desktop_capture {
	struct screencopy base;
	struct desktop* desktop;
	bool render_cursor;
};

struct desktop* desktop_from_image_source(const struct image_source* source);

struct desktop* desktop_new(struct wl_list* output_list);
void desktop_destroy(struct desktop* self);
