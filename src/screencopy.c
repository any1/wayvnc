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
#include "frame-capture.h"
#include "shm.h"
#include "screencopy.h"
#include "smooth.h"
#include "time-util.h"
#include "render.h"
#include "usdt.h"

#define RATE_LIMIT 20.0 // Hz
#define DELAY_SMOOTHER_TIME_CONSTANT 0.5 // s

static uint32_t fourcc_from_wl_shm(enum wl_shm_format in)
{
	switch (in) {
	case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
	default: return in;
	}
}

static void screencopy_stop(struct frame_capture* fc)
{
	struct screencopy* self = (void*)fc;

	aml_stop(aml_get_default(), self->timer);

	self->frame_capture.status = CAPTURE_STOPPED;

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

	uint32_t fourcc = fourcc_from_wl_shm(format);
	wv_buffer_pool_resize(self->pool, width, height, stride, fourcc);

	struct wv_buffer* buffer = wv_buffer_pool_acquire(self->pool);
	if (!buffer) {
		self->frame_capture.status = CAPTURE_FATAL;
		screencopy_stop(&self->frame_capture);
		self->frame_capture.on_done(&self->frame_capture);
	}

	buffer->y_inverted = true;

	assert(!self->front);
	self->front = buffer;

	self->frame_capture.frame_info.fourcc_format = fourcc;
	self->frame_capture.frame_info.width = width;
	self->frame_capture.frame_info.height = height;
	self->frame_capture.frame_info.stride = stride;

	if (self->is_immediate_copy)
		zwlr_screencopy_frame_v1_copy(self->frame, buffer->wl_buffer);
	else
		zwlr_screencopy_frame_v1_copy_with_damage(self->frame,
				buffer->wl_buffer);
}

static void screencopy_flags(void* data,
			     struct zwlr_screencopy_frame_v1* frame,
			     uint32_t flags)
{
	struct screencopy* self = data;
	(void)frame;

//	self->buffer->y_inverted = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
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

	screencopy_stop(&self->frame_capture);

	self->last_time = gettime_us();

	double delay = (self->last_time - self->start_time) * 1.0e-6;
	self->delay = smooth(&self->delay_smoother, delay);

	if (self->is_immediate_copy) {
		self->frame_capture.damage_hint.x = 0;
		self->frame_capture.damage_hint.y = 0;
		self->frame_capture.damage_hint.width =
			self->frame_capture.frame_info.width;
		self->frame_capture.damage_hint.height =
			self->frame_capture.frame_info.height;
	}

	if (self->back)
		wv_buffer_pool_release(self->pool, self->back);
	self->back = self->front;
	self->front = NULL;

	self->frame_capture.status = CAPTURE_DONE;
	self->frame_capture.on_done(&self->frame_capture);
}

static void screencopy_failed(void* data,
			      struct zwlr_screencopy_frame_v1* frame)
{
	struct screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_failed, self);

	screencopy_stop(&self->frame_capture);
	self->frame_capture.status = CAPTURE_FAILED;
	self->frame_capture.on_done(&self->frame_capture);
}

static void screencopy_damage(void* data,
			      struct zwlr_screencopy_frame_v1* frame,
			      uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height)
{
	struct screencopy* self = data;

	DTRACE_PROBE1(wayvnc, screencopy_damage, self);

	self->frame_capture.damage_hint.x = x;
	self->frame_capture.damage_hint.y = y;
	self->frame_capture.damage_hint.width = width;
	self->frame_capture.damage_hint.height = height;
}

static int screencopy__start_capture(struct frame_capture* fc)
{
	struct screencopy* self = (void*)fc;

	DTRACE_PROBE1(wayvnc, screencopy_start, self);

	static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
		.buffer = screencopy_buffer,
		.flags = screencopy_flags,
		.ready = screencopy_ready,
		.failed = screencopy_failed,
		.damage = screencopy_damage,
	};

	self->start_time = gettime_us();

	self->frame =
		zwlr_screencopy_manager_v1_capture_output(self->manager, 
							  fc->overlay_cursor,
							  fc->wl_output);
	if (!self->frame)
		return -1;

	zwlr_screencopy_frame_v1_add_listener(self->frame, &frame_listener,
					      self);

	return 0;
}

static void screencopy__poll(void* obj)
{
	struct screencopy* self = aml_get_userdata(obj);
	struct frame_capture* fc = (struct frame_capture*)self;

	screencopy__start_capture(fc);
}

static int screencopy_start(struct frame_capture* fc,
                            enum frame_capture_options options)
{
	struct screencopy* self = (void*)fc;

	if (fc->status == CAPTURE_IN_PROGRESS)
		return -1;

	self->is_immediate_copy = !!(options & CAPTURE_NOW);

	uint64_t now = gettime_us();
	double dt = (now - self->last_time) * 1.0e-6;
	double time_left = (1.0 / RATE_LIMIT - dt - self->delay) * 1.0e3;

	fc->status = CAPTURE_IN_PROGRESS;

	if (time_left > 0) {
		aml_set_duration(self->timer, time_left);
		return aml_start(aml_get_default(), self->timer);
	}

	return screencopy__start_capture(fc);
}

static void screencopy_render(struct frame_capture* fc,
                              struct renderer* renderer, struct nvnc_fb* fb)
{
	/*
	uint32_t width = fc->frame_info.width;
	uint32_t height = fc->frame_info.height;
	uint32_t stride = fc->frame_info.stride;
	uint32_t format = fc->frame_info.fourcc_format;

	render_framebuffer(renderer, self->pixels, format, width, height, stride);
	*/
}

void screencopy_init(struct screencopy* self)
{
	self->pool = wv_buffer_pool_create(0, 0, 0, 0);
	assert(self->pool);

	self->timer = aml_timer_new(0, screencopy__poll, self, NULL);
	assert(self->timer);

	self->delay_smoother.time_constant = DELAY_SMOOTHER_TIME_CONSTANT;

	self->frame_capture.backend.start = screencopy_start;
	self->frame_capture.backend.stop = screencopy_stop;
	self->frame_capture.backend.render = screencopy_render;
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
