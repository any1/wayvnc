/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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
#include <inttypes.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <time-util.h>

#include "frame-capture.h"
#include "logging.h"
#include "dmabuf.h"
#include "wlr-export-dmabuf-unstable-v1.h"
#include "render.h"
#include "usdt.h"

#define PROCESSING_DURATION_LIMIT 16 /* ms */

static void dmabuf_close_fds(struct dmabuf_capture* self)
{
	for (size_t i = 0; i < self->frame.n_planes; ++i)
		close(self->frame.plane[i].fd);

	self->frame.n_planes = 0;
}

static void dmabuf_capture_stop(struct frame_capture* fc)
{
	struct dmabuf_capture* self = (void*)fc;

	fc->status = CAPTURE_STOPPED;

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

	dmabuf_close_fds(self);

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

	struct timespec ts = {
		.tv_sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo,
		.tv_nsec = tv_nsec
	};

	uint64_t precommit_time = timespec_to_ms(&ts);

	DTRACE_PROBE2(wayvnc, dmabuf_frame_ready, self, precommit_time);

	dmabuf_capture_stop(fc);

	fc->status = CAPTURE_DONE;
	fc->on_done(fc);

	dmabuf_close_fds(self);

	uint64_t processing_duration =
		self->render_finish_time - precommit_time;

	DTRACE_PROBE3(wayvnc, dmabuf_frame_release, self,
	              self->render_finish_time, processing_duration);

	if (processing_duration > PROCESSING_DURATION_LIMIT)
		log_debug("Processing dmabuf took %"PRIu64" ms.\n",
		          processing_duration);
}

static void dmabuf_frame_cancel(void* data,
				struct zwlr_export_dmabuf_frame_v1* frame,
				uint32_t reason)
{
	struct dmabuf_capture* self = data;
	struct frame_capture* fc = data;

	DTRACE_PROBE1(wayvnc, dmabuf_frame_cancel, self);

	dmabuf_capture_stop(fc);
	fc->status = reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT
		     ? CAPTURE_FATAL : CAPTURE_FAILED;

	fc->on_done(fc);

	dmabuf_close_fds(self);
}

static int dmabuf_capture_start(struct frame_capture* fc,
                                enum frame_capture_options options)
{
	struct dmabuf_capture* self = (void*)fc;

	static const struct zwlr_export_dmabuf_frame_v1_listener
	dmabuf_frame_listener = {
		.frame = dmabuf_frame_start,
		.object = dmabuf_frame_object,
		.ready = dmabuf_frame_ready,
		.cancel = dmabuf_frame_cancel,
	};

	DTRACE_PROBE1(wayvnc, dmabuf_capture_start, self);

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

static void dmabuf_capture_render(struct frame_capture* fc,
                                  struct renderer* render, struct nvnc_fb* fb)
{
	struct dmabuf_capture* self = (void*)fc;
	render_dmabuf(render, &self->frame);
	self->render_finish_time = gettime_ms();
}

void dmabuf_capture_init(struct dmabuf_capture* self)
{
	self->fc.backend.start = dmabuf_capture_start;
	self->fc.backend.stop = dmabuf_capture_stop;
	self->fc.backend.render = dmabuf_capture_render;
}
