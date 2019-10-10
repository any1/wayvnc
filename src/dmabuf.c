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
#include <wayland-client.h>

#include "logging.h"
#include "dmabuf.h"
#include "wlr-export-dmabuf-unstable-v1.h"

static void dmabuf_capture_stop(struct frame_capture* fc)
{
	struct dmabuf_capture* self = (void*)fc;

	if (self->zwlr_frame) {
		zwlr_export_dmabuf_frame_v1_destroy(self->zwlr_frame);
		self->zwlr_frame = NULL;
	}
}

static void dmabuf_frame_start(void* data,
			       struct zwlr_export_dmabuf_frame_v1* frame,
			       uint32_t width, uint32_t height,
			       uint32_t offset_x, uint32_t offset_y,
			       uint32_t buffer_flags, uint32_t flags,
			       uint32_t format,
			       uint32_t mod_high, uint32_t mod_low,
			       uint32_t num_objects)
{
	struct dmabuf_capture* self = data;
	struct frame_capture* fc = data;

	uint64_t mod = ((uint64_t)mod_high << 32) | (uint64_t)mod_low;

	self->frame.width = width;
	self->frame.height = height;
	self->frame.n_planes = num_objects;
	self->frame.format = format;

	self->frame.plane[0].modifier = mod;
	self->frame.plane[1].modifier = mod;
	self->frame.plane[2].modifier = mod;
	self->frame.plane[3].modifier = mod;

	fc->damage_hint.x = 0;
	fc->damage_hint.y = 0;
	fc->damage_hint.width = width;
	fc->damage_hint.height = height;
}

static void dmabuf_frame_object(void* data,
				struct zwlr_export_dmabuf_frame_v1* frame,
				uint32_t index, int32_t fd, uint32_t size, 
				uint32_t offset, uint32_t stride,
				uint32_t plane_index)
{
	struct dmabuf_capture* self = data;
	struct frame_capture* fc = data;

	self->frame.plane[plane_index].fd = fd;
	self->frame.plane[plane_index].size = size;
	self->frame.plane[plane_index].offset = offset;
	self->frame.plane[plane_index].pitch = stride;
}

static void dmabuf_frame_ready(void* data,
			       struct zwlr_export_dmabuf_frame_v1* frame,
			       uint32_t tv_sec_hi, uint32_t tv_sec_lo,
			       uint32_t tv_nsec)
{
	struct dmabuf_capture* self = data;
	struct frame_capture* fc = data;

	dmabuf_capture_stop(fc);

	fc->status = CAPTURE_DONE;
	fc->on_done(fc);
}

static void dmabuf_frame_cancel(void* data,
				struct zwlr_export_dmabuf_frame_v1* frame,
				uint32_t reason)
{
	struct frame_capture* fc = data;

	fc->status = reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT
		     ? CAPTURE_FATAL : CAPTURE_FAILED;

	dmabuf_capture_stop(fc);
	fc->on_done(fc);
}

static int dmabuf_capture_start(struct frame_capture* fc)
{
	struct dmabuf_capture* self = (void*)fc;

	static const struct zwlr_export_dmabuf_frame_v1_listener
	dmabuf_frame_listener = {
		.frame = dmabuf_frame_start,
		.object = dmabuf_frame_object,
		.ready = dmabuf_frame_ready,
		.cancel = dmabuf_frame_cancel,
	};

	self->zwlr_frame =
		zwlr_export_dmabuf_manager_v1_capture_output(self->manager,
							     fc->overlay_cursor,
							     fc->wl_output);
	if (!self->zwlr_frame)
		return -1;

	fc->status = CAPTURE_IN_PROGRESS;

	zwlr_export_dmabuf_frame_v1_add_listener(self->zwlr_frame,
			&dmabuf_frame_listener, self);

	return 0;
}

void dmabuf_capture_init(struct dmabuf_capture* self)
{
	self->fc.backend.start = dmabuf_capture_start;
	self->fc.backend.stop = dmabuf_capture_stop;
}
