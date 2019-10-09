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

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <wayland-client.h>

#include "shm.h"
#include "screencopy.h"

static int screencopy_buffer_init(struct screencopy* self,
				  enum wl_shm_format format, uint32_t width,
				  uint32_t height, uint32_t stride)
{
	if (self->buffer)
		return 0;

	size_t size = stride * height;

	int fd = shm_alloc_fd(size);
	if (fd < 0)
		return -1;

	struct wl_shm_pool* pool = wl_shm_create_pool(self->wl_shm, fd, size);
	close(fd);
	if (!pool)
		return -1;

	struct wl_buffer* buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					  format);
	wl_shm_pool_destroy(pool);

	self->buffer = buffer;
	return 0;
}

void screencopy_stop(struct screencopy* self)
{
	if (self->frame) {
		zwlr_screencopy_frame_v1_destroy(self->frame);
		self->frame = NULL;
	}
}

static void screencopy_buffer(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      enum wl_shm_format format, uint32_t width,
			      uint32_t height, uint32_t stride)
{
	struct screencopy* self = data;

	if (screencopy_buffer_init(self, format, width, height, stride) < 0) {
		self->status = SCREENCOPY_STATUS_FATAL;
		screencopy_stop(self);
		self->on_done(self);
	}

	zwlr_screencopy_frame_v1_copy(self->frame, self->buffer);
}

static void screencopy_flags(void* data,
			     struct zwlr_screencopy_frame_v1* frame,
			     uint32_t flags)
{
	(void)data;
	(void)frame;
	(void)flags;

	/* TODO. Assume y-invert for now */
}

static void screencopy_ready(void* data,
			     struct zwlr_screencopy_frame_v1* frame,
			     uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	(void)sec_hi;
	(void)sec_lo;
	(void)nsec;

	struct screencopy* self = data;
	screencopy_stop(self);
	self->status = SCREENCOPY_STATUS_DONE;
	self->on_done(self);
}

static void screencopy_failed(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct screencopy* self = data;
	screencopy_stop(self);
	self->status = SCREENCOPY_STATUS_FAILED;
	self->on_done(self);
}

static void screencopy_damage(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height)
{
	struct screencopy* self = data;

	/* TODO */
}

int screencopy_start(struct screencopy* self)
{
	static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
		.buffer = screencopy_buffer,
		.flags = screencopy_flags,
		.ready = screencopy_ready,
		.failed = screencopy_failed,
		.damage = screencopy_damage,
	};

	self->frame =
		zwlr_screencopy_manager_v1_capture_output(self->manager, 
							  self->overlay_cursor, 
							  self->output);
	if (!self->frame)
		return -1;

	zwlr_screencopy_frame_v1_add_listener(self->frame, &frame_listener,
					      self);

	self->status = SCREENCOPY_STATUS_CAPTURING;
	return 0;
}
