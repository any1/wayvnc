/*
 * Copyright (c) 2022 - 2025 Andri Yngvason
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

#include "screencopy-interface.h"

#include <unistd.h>

extern struct zwlr_screencopy_manager_v1* screencopy_manager;
extern struct ext_output_image_capture_source_manager_v1*
		ext_output_image_capture_source_manager;
extern struct ext_image_copy_capture_manager_v1* ext_image_copy_capture_manager;

extern struct screencopy_impl wlr_screencopy_impl;
extern struct screencopy_impl ext_image_copy_capture_impl;

struct screencopy* screencopy_create(struct image_source* source,
		bool render_cursor)
{
	if (ext_image_copy_capture_manager && ext_output_image_capture_source_manager)
		return ext_image_copy_capture_impl.create(source, render_cursor);
	if (screencopy_manager)
		return wlr_screencopy_impl.create(source, render_cursor);
	return NULL;
}

struct screencopy* screencopy_create_cursor(struct image_source* source,
		struct wl_seat* seat)
{
	if (ext_image_copy_capture_manager && ext_output_image_capture_source_manager)
		return ext_image_copy_capture_impl.create_cursor(source, seat);
	return NULL;
}

void screencopy_destroy(struct screencopy* self)
{
	if (self)
		self->impl->destroy(self);
}

int screencopy_start(struct screencopy* self, bool immediate)
{
	return self->impl->start(self, immediate);
}

void screencopy_stop(struct screencopy* self)
{
	if (self)
		self->impl->stop(self);
}
