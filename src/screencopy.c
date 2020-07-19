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
#include <stdbool.h>
#include <sys/mman.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>

#include "wlr-screencopy-unstable-v1.h"
#include "buffer.h"
#include "shm.h"
#include "screencopy.h"
#include "smooth.h"
#include "time-util.h"
#include "usdt.h"
#include "pixels.h"

#define DELAY_SMOOTHER_TIME_CONSTANT 0.5 // s

static void screencopy__stop(struct screencopy* self)
{
	aml_stop(aml_get_default(), self->timer);

	self->status = SCREENCOPY_STOPPED;

	if (self->frame) {
		zwlr_screencopy_frame_v1_destroy(self->frame);
		self->frame = NULL;
	}
}

void screencopy_stop(struct screencopy* self)
{
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
	struct screencopy* self = data;

	self->have_linux_dmabuf = true;
	self->dmabuf_width = width;
	self->dmabuf_height = height;
	self->fourcc = format;
#endif
}

static void screencopy_buffer_done(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct screencopy* self = data;
	uint32_t width, height, stride, fourcc;
	enum wv_buffer_type type = WV_BUFFER_UNSPEC;

	if (self->have_linux_dmabuf) {
		width = self->dmabuf_width;
		height = self->dmabuf_height;
		stride = 0;
		fourcc = self->fourcc;
		type = WV_BUFFER_DMABUF;
	} else {
		width = self->wl_shm_width;
		height = self->wl_shm_height;
		stride = self->wl_shm_stride;
		fourcc = fourcc_from_wl_shm(self->wl_shm_format);
		type = WV_BUFFER_SHM;
	}

	wv_buffer_pool_resize(self->pool, type, width, height, stride, fourcc);

	struct wv_buffer* buffer = wv_buffer_pool_acquire(self->pool);
	if (!buffer) {
		screencopy__stop(self);
		self->status = SCREENCOPY_FATAL;
		self->on_done(self);
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
	struct screencopy* self = data;

	self->wl_shm_format = format;
	self->wl_shm_width = width;
	self->wl_shm_height = height;
	self->wl_shm_stride = stride;

	if (self->version < 3) {
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

	struct screencopy* self = data;

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

	struct screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_ready, self);

	screencopy__stop(self);

	self->last_time = gettime_us();

	double delay = (self->last_time - self->start_time) * 1.0e-6;
	self->delay = smooth(&self->delay_smoother, delay);

	if (self->is_immediate_copy)
		wv_buffer_damage_whole(self->front);

	if (self->back)
		wv_buffer_pool_release(self->pool, self->back);
	self->back = self->front;
	self->front = NULL;

	wv_buffer_map(self->back);

	self->status = SCREENCOPY_DONE;
	self->on_done(self);
}

static void screencopy_failed(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_failed, self);

	screencopy__stop(self);

	wv_buffer_pool_release(self->pool, self->front);
	self->front = NULL;

	self->status = SCREENCOPY_FAILED;
	self->on_done(self);
}

static void screencopy_damage(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height)
{
	struct screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_damage, self);

	wv_buffer_damage_rect(self->front, x, y, width, height);
}

static int screencopy__start_capture(struct screencopy* self)
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

	self->start_time = gettime_us();

	self->frame = zwlr_screencopy_manager_v1_capture_output(self->manager,
			self->overlay_cursor, self->wl_output);
	if (!self->frame)
		return -1;

	zwlr_screencopy_frame_v1_add_listener(self->frame, &frame_listener,
					      self);

	return 0;
}

static void screencopy__poll(void* obj)
{
	struct screencopy* self = aml_get_userdata(obj);

	screencopy__start_capture(self);
}

static int screencopy__start(struct screencopy* self, bool is_immediate_copy)
{
	if (self->status == SCREENCOPY_IN_PROGRESS)
		return -1;

	self->is_immediate_copy = is_immediate_copy;

	uint64_t now = gettime_us();
	double dt = (now - self->last_time) * 1.0e-6;
	int32_t time_left = (1.0 / self->rate_limit - dt - self->delay) * 1.0e3;

	self->status = SCREENCOPY_IN_PROGRESS;

	if (time_left > 0) {
		aml_set_duration(self->timer, time_left);
		return aml_start(aml_get_default(), self->timer);
	}

	return screencopy__start_capture(self);
}

int screencopy_start(struct screencopy* self)
{
	return screencopy__start(self, false);
}

int screencopy_start_immediate(struct screencopy* self)
{
	return screencopy__start(self, true);
}

void screencopy_init(struct screencopy* self)
{
	self->pool = wv_buffer_pool_create(0, 0, 0, 0, 0);
	assert(self->pool);

	self->timer = aml_timer_new(0, screencopy__poll, self, NULL);
	assert(self->timer);

	self->delay_smoother.time_constant = DELAY_SMOOTHER_TIME_CONSTANT;
}

void screencopy_destroy(struct screencopy* self)
{
	aml_stop(aml_get_default(), self->timer);
	aml_unref(self->timer);

	if (self->back)
		wv_buffer_pool_release(self->pool, self->back);
	if (self->front)
		wv_buffer_pool_release(self->pool, self->front);

	wv_buffer_pool_destroy(self->pool);
}
