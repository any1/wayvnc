/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
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
#include <sys/mman.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>
#include <neatvnc.h>

#include "wlr-screencopy-unstable-v1.h"
#include "buffer.h"
#include "shm.h"
#include "screencopy-interface.h"
#include "time-util.h"
#include "usdt.h"
#include "pixels.h"
#include "config.h"
#include "image-source.h"
#include "output.h"

extern struct zwlr_screencopy_manager_v1* screencopy_manager;

enum wlr_screencopy_status {
	WLR_SCREENCOPY_STOPPED = 0,
	WLR_SCREENCOPY_IN_PROGRESS,
	WLR_SCREENCOPY_FAILED,
	WLR_SCREENCOPY_FATAL,
	WLR_SCREENCOPY_DONE,
};

struct wlr_screencopy {
	struct screencopy parent;

	enum wlr_screencopy_status status;

	struct wv_buffer_pool* pool;
	struct wv_buffer* front;
	struct wv_buffer* back;

	struct zwlr_screencopy_frame_v1* frame;

	uint64_t last_time;
	struct aml_timer* timer;

	bool is_immediate_copy;
	bool overlay_cursor;
	struct output* output;

	uint32_t wl_shm_width, wl_shm_height, wl_shm_stride;
	enum wl_shm_format wl_shm_format;

	bool have_linux_dmabuf;
	uint32_t dmabuf_width, dmabuf_height;
	uint32_t fourcc;
};

struct screencopy_impl wlr_screencopy_impl;

static void screencopy__stop(struct wlr_screencopy* self)
{
	aml_stop(aml_get_default(), self->timer);

	self->status = WLR_SCREENCOPY_STOPPED;

	if (self->frame) {
		zwlr_screencopy_frame_v1_destroy(self->frame);
		self->frame = NULL;
	}
}

void wlr_screencopy_stop(struct screencopy* ptr)
{
	struct wlr_screencopy* self = (struct wlr_screencopy*)ptr;

	if (self->front)
		wv_buffer_pool_release(self->pool, self->front);
	self->front = NULL;

	return screencopy__stop(self);
}

static void screencopy_linux_dmabuf(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      uint32_t format, uint32_t width, uint32_t height)
{
#ifdef ENABLE_SCREENCOPY_DMABUF
	struct wlr_screencopy* self = data;

	if (!(wv_buffer_get_available_types() & WV_BUFFER_DMABUF))
		return;

	self->have_linux_dmabuf = true;
	self->dmabuf_width = width;
	self->dmabuf_height = height;
	self->fourcc = format;
#endif
}

static void screencopy_buffer_done(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct wlr_screencopy* self = data;
	struct wv_buffer_config config = {};

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (self->have_linux_dmabuf && self->parent.enable_linux_dmabuf) {
		config.width = self->dmabuf_width;
		config.height = self->dmabuf_height;
		config.stride = 0;
		config.format = self->fourcc;
		config.type = WV_BUFFER_DMABUF;
	} else
#endif
	{
		config.width = self->wl_shm_width;
		config.height = self->wl_shm_height;
		config.stride = self->wl_shm_stride;
		config.format = fourcc_from_wl_shm(self->wl_shm_format);
		config.type = WV_BUFFER_SHM;
	}

	wv_buffer_pool_reconfig(self->pool, &config);

	struct wv_buffer* buffer = wv_buffer_pool_acquire(self->pool);
	if (!buffer) {
		screencopy__stop(self);
		self->status = WLR_SCREENCOPY_FATAL;
		self->parent.on_done(SCREENCOPY_FATAL, NULL,
				&self->output->image_source,
				self->parent.userdata);
		return;
	}

	assert(!self->front);
	self->front = buffer;

	if (self->is_immediate_copy)
		zwlr_screencopy_frame_v1_copy(self->frame, buffer->wl_buffer);
	else
		zwlr_screencopy_frame_v1_copy_with_damage(self->frame,
				buffer->wl_buffer);
}

static void screencopy_buffer(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      enum wl_shm_format format, uint32_t width,
			      uint32_t height, uint32_t stride)
{
	struct wlr_screencopy* self = data;

	self->wl_shm_format = format;
	self->wl_shm_width = width;
	self->wl_shm_height = height;
	self->wl_shm_stride = stride;

	int version = zwlr_screencopy_manager_v1_get_version(screencopy_manager);
	if (version < 3) {
		self->have_linux_dmabuf = false;
		screencopy_buffer_done(data, frame);
		return;
	}
}

static void screencopy_flags(void* data,
			     struct zwlr_screencopy_frame_v1* frame,
			     uint32_t flags)
{
	(void)frame;

	struct wlr_screencopy* self = data;

	self->front->y_inverted =
		!!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}

static void screencopy_ready(void* data,
			     struct zwlr_screencopy_frame_v1* frame,
			     uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	(void)sec_hi;
	(void)sec_lo;
	(void)nsec;

	struct wlr_screencopy* self = data;

	uint64_t sec = (uint64_t)sec_hi << 32 | (uint64_t)sec_lo;
	uint64_t pts = sec * UINT64_C(1000000) + (uint64_t)nsec / UINT64_C(1000);

	DTRACE_PROBE2(wayvnc, screencopy_ready, self, pts);

	screencopy__stop(self);

	if (self->is_immediate_copy)
		wv_buffer_damage_whole(self->front);

	if (self->back)
		wv_buffer_pool_release(self->pool, self->back);
	self->back = self->front;
	self->front = NULL;

	nvnc_fb_set_pts(self->back->nvnc_fb, pts);

	self->status = WLR_SCREENCOPY_DONE;
	self->parent.on_done(SCREENCOPY_DONE, self->back,
			&self->output->image_source, self->parent.userdata);

	self->back = NULL;
}

static void screencopy_failed(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct wlr_screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_failed, self);

	screencopy__stop(self);

	if (self->front)
		wv_buffer_pool_release(self->pool, self->front);
	self->front = NULL;

	self->status = WLR_SCREENCOPY_FAILED;
	self->parent.on_done(SCREENCOPY_FAILED, NULL,
			&self->output->image_source, self->parent.userdata);
}

static void screencopy_damage(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height)
{
	struct wlr_screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_damage, self);

	wv_buffer_damage_rect(self->front, x, y, width, height);
}

static int screencopy__start_capture(struct wlr_screencopy* self, uint64_t now)
{
	DTRACE_PROBE1(wayvnc, screencopy_start, self);

	static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
		.buffer = screencopy_buffer,
		.linux_dmabuf = screencopy_linux_dmabuf,
		.buffer_done = screencopy_buffer_done,
		.flags = screencopy_flags,
		.ready = screencopy_ready,
		.failed = screencopy_failed,
		.damage = screencopy_damage,
	};

	self->frame = zwlr_screencopy_manager_v1_capture_output(
			screencopy_manager, self->overlay_cursor,
			self->output->wl_output);
	if (!self->frame)
		return -1;

	zwlr_screencopy_frame_v1_add_listener(self->frame, &frame_listener,
					      self);

	self->last_time = now;

	return 0;
}

static void screencopy__poll(struct aml_timer* handler)
{
	struct wlr_screencopy* self = aml_get_userdata(handler);
	uint64_t now = gettime_us();
	screencopy__start_capture(self, now);
}

static int wlr_screencopy_start(struct screencopy* ptr, bool is_immediate_copy)
{
	struct wlr_screencopy* self = (struct wlr_screencopy*)ptr;

	if (self->status == WLR_SCREENCOPY_IN_PROGRESS)
		return 0;

	self->is_immediate_copy = is_immediate_copy;

	uint64_t now = gettime_us();
	double dt = (now - self->last_time) * 1.0e-6;
	int32_t time_left = (1.0 / ptr->rate_limit - dt) * 1.0e6;

	self->status = WLR_SCREENCOPY_IN_PROGRESS;

	if (time_left > 0) {
		aml_set_duration(self->timer, time_left);
		return aml_start(aml_get_default(), self->timer);
	}

	return screencopy__start_capture(self, now);
}

static struct screencopy* wlr_screencopy_create(struct image_source* source,
		bool render_cursor)
{
	if (!image_source_is_output(source)) {
		nvnc_log(NVNC_LOG_ERROR, "Missing features for non-output capture");
		return NULL;
	}

	struct wlr_screencopy* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &wlr_screencopy_impl;
	self->parent.rate_limit = 30;

	self->output = output_from_image_source(source);
	self->overlay_cursor = render_cursor;

	self->pool = wv_buffer_pool_create(NULL);
	assert(self->pool);

	self->timer = aml_timer_new(0, screencopy__poll, self, NULL);
	assert(self->timer);

	return (struct screencopy*)self;
}

static void wlr_screencopy_destroy(struct screencopy* ptr)
{
	struct wlr_screencopy* self = (struct wlr_screencopy*)ptr;
	aml_stop(aml_get_default(), self->timer);
	aml_unref(self->timer);

	if (self->back)
		wv_buffer_pool_release(self->pool, self->back);
	if (self->front)
		wv_buffer_pool_release(self->pool, self->front);

	self->back = NULL;
	self->front = NULL;

	wv_buffer_pool_destroy(self->pool);
	free(self);
}

struct screencopy_impl wlr_screencopy_impl = {
	.create = wlr_screencopy_create,
	.destroy = wlr_screencopy_destroy,
	.start = wlr_screencopy_start,
	.stop = wlr_screencopy_stop,
};
