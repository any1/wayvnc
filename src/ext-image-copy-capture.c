/*
 * Copyright (c) 2022 - 2024 Andri Yngvason
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
#include <inttypes.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/param.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>
#include <neatvnc.h>

#include "screencopy-interface.h"
#include "ext-image-copy-capture-v1.h"
#include "ext-image-capture-source-v1.h"
#include "buffer.h"
#include "shm.h"
#include "time-util.h"
#include "usdt.h"
#include "pixels.h"
#include "config.h"

extern struct ext_output_image_capture_source_manager_v1* ext_output_image_capture_source_manager;
extern struct ext_image_copy_capture_manager_v1* ext_image_copy_capture_manager;

struct dmabuf_format {
	uint32_t format;
	uint64_t* modifiers;
	int n_modifiers;
};

struct ext_image_copy_capture {
	struct screencopy parent;
	struct wl_output* wl_output;
	struct wl_seat* wl_seat;
	struct ext_image_copy_capture_session_v1* session;
	struct ext_image_copy_capture_frame_v1* frame;
	struct ext_image_copy_capture_cursor_session_v1* cursor;
	bool render_cursors;
	struct wv_buffer_pool* pool;
	struct wv_buffer* buffer;
	bool have_constraints;
	bool should_start;
	uint32_t frame_count;

	uint32_t* wl_shm_formats;
	int n_wl_shm_formats;
	int wl_shm_formats_capacity;

	uint32_t width, height;
	uint32_t wl_shm_stride;

	struct dmabuf_format* dmabuf_formats;
	int n_dmabuf_formats;
	int dmabuf_formats_capacity;

	bool have_dmabuf_dev;
	dev_t dmabuf_dev;

	struct { int x, y; } hotspot;

	uint64_t last_time;
	struct aml_timer* timer;
};

struct screencopy_impl ext_image_copy_capture_impl;

static struct ext_image_copy_capture_session_v1_listener session_listener;
static struct ext_image_copy_capture_frame_v1_listener frame_listener;
static struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener;

static void clear_dmabuf_formats(struct ext_image_copy_capture* self)
{
	for (int i = 0; i < self->n_dmabuf_formats; ++i)
		free(self->dmabuf_formats[i].modifiers);
	self->n_dmabuf_formats = 0;
}

static void clear_constraints(struct ext_image_copy_capture* self)
{
	if (!self->have_constraints)
		return;

	clear_dmabuf_formats(self);
	self->n_dmabuf_formats = 0;
	self->n_wl_shm_formats = 0;
	self->have_constraints = false;
}

static void ext_image_copy_capture_deinit_session(struct ext_image_copy_capture* self)
{
	nvnc_log(NVNC_LOG_DEBUG, "DEINIT %p", self);

	clear_constraints(self);

	if (self->frame)
		ext_image_copy_capture_frame_v1_destroy(self->frame);
	self->frame = NULL;

	if (self->session)
		ext_image_copy_capture_session_v1_destroy(self->session);
	self->session = NULL;

	if (self->cursor)
		ext_image_copy_capture_cursor_session_v1_destroy(self->cursor);
	self->cursor = NULL;

	if (self->buffer)
		wv_buffer_pool_release(self->pool, self->buffer);
	self->buffer = NULL;
}

static int ext_image_copy_capture_init_session(struct ext_image_copy_capture* self)
{
	struct ext_image_capture_source_v1* source;
	source = ext_output_image_capture_source_manager_v1_create_source(
			ext_output_image_capture_source_manager, self->wl_output);
	if (!source)
		return -1;

	enum ext_image_copy_capture_manager_v1_options options = 0;
	if (self->render_cursors)
		options |= EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;

	self->session = ext_image_copy_capture_manager_v1_create_session(
			ext_image_copy_capture_manager, source, options);
	ext_image_capture_source_v1_destroy(source);
	if (!self->session)
		return -1;

	ext_image_copy_capture_session_v1_add_listener(self->session,
			&session_listener, self);
	return 0;
}

static int ext_image_copy_capture_init_cursor_session(struct ext_image_copy_capture* self)
{
	struct ext_image_capture_source_v1* source;
	source = ext_output_image_capture_source_manager_v1_create_source(
			ext_output_image_capture_source_manager, self->wl_output);
	if (!source)
		return -1;

	struct wl_pointer* pointer = wl_seat_get_pointer(self->wl_seat);
	self->cursor = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
			ext_image_copy_capture_manager, source, pointer);
	ext_image_capture_source_v1_destroy(source);
	wl_pointer_release(pointer);
	if (!self->cursor)
		return -1;

	ext_image_copy_capture_cursor_session_v1_add_listener(self->cursor,
			&cursor_listener, self);

	self->session = ext_image_copy_capture_cursor_session_v1_get_capture_session(
			self->cursor);
	assert(self->session);

	ext_image_copy_capture_session_v1_add_listener(self->session,
			&session_listener, self);

	return 0;
}

static void ext_image_copy_capture_schedule_capture(struct ext_image_copy_capture* self)
{
	assert(!self->frame);

	self->buffer = wv_buffer_pool_acquire(self->pool);
	self->buffer->domain = self->cursor ? WV_BUFFER_DOMAIN_CURSOR :
		WV_BUFFER_DOMAIN_OUTPUT;

	self->frame = ext_image_copy_capture_session_v1_create_frame(self->session);
	assert(self->frame);

	ext_image_copy_capture_frame_v1_attach_buffer(self->frame,
			self->buffer->wl_buffer);
	ext_image_copy_capture_frame_v1_add_listener(self->frame, &frame_listener,
			self);

	int n_rects = 0;
	struct pixman_box16* rects =
		pixman_region_rectangles(&self->buffer->buffer_damage, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		uint32_t x = rects[i].x1;
		uint32_t y = rects[i].y1;
		uint32_t width = rects[i].x2 - x;
		uint32_t height = rects[i].y2 - y;

		ext_image_copy_capture_frame_v1_damage_buffer(self->frame, x, y,
				width, height);
	}

	ext_image_copy_capture_frame_v1_capture(self->frame);

	float damage_area = calculate_region_area(&self->buffer->buffer_damage);
	float pixel_area = self->buffer->width * self->buffer->height;

	nvnc_trace("Committed %sbuffer: %p with %.02f %% damage",
			self->cursor ? "cursor " : "", self->buffer,
			100.0 * damage_area / pixel_area);
}

static void ext_image_copy_capture_schedule_from_timer(void* obj)
{
	struct ext_image_copy_capture* self = aml_get_userdata(obj);
	assert(self);

	ext_image_copy_capture_schedule_capture(self);
}

static void session_handle_format_shm(void *data,
		struct ext_image_copy_capture_session_v1* session,
		uint32_t format)
{
	struct ext_image_copy_capture* self = data;

	clear_constraints(self);

	if (self->wl_shm_formats_capacity <= self->n_wl_shm_formats) {
		int next_cap = MIN(256, self->wl_shm_formats_capacity * 2);
		uint32_t* formats = realloc(self->wl_shm_formats,
				sizeof(*formats) * next_cap);
		assert(formats);

		self->wl_shm_formats = formats;
		self->wl_shm_formats_capacity = next_cap;
	}

	self->wl_shm_formats[self->n_wl_shm_formats++] =
		fourcc_from_wl_shm(format);

	nvnc_log(NVNC_LOG_DEBUG, "shm format: %"PRIx32, format);
}

static void session_handle_format_drm(void *data,
		struct ext_image_copy_capture_session_v1 *session,
		uint32_t format, struct wl_array* modifiers)
{
#ifdef ENABLE_SCREENCOPY_DMABUF
	struct ext_image_copy_capture* self = data;

	clear_constraints(self);

	nvnc_log(NVNC_LOG_DEBUG, "DMA-BUF format: %"PRIx32, format);

	if (self->dmabuf_formats_capacity <= self->n_dmabuf_formats) {
		int next_cap = MIN(256, self->dmabuf_formats_capacity * 2);
		struct dmabuf_format* formats = realloc(self->dmabuf_formats,
				sizeof(*formats) * next_cap);
		assert(formats);

		self->dmabuf_formats = formats;
		self->dmabuf_formats_capacity = next_cap;
	}

	struct dmabuf_format* entry =
		&self->dmabuf_formats[self->n_dmabuf_formats++];

	entry->format = format;

	if (modifiers->size % 8 != 0) {
		nvnc_log(NVNC_LOG_WARNING, "DMA-BUF modifier array size is not a multiple of 8");
	}

	entry->n_modifiers = modifiers->size / 8;
	entry->modifiers = realloc(entry->modifiers, entry->n_modifiers * 8);
	assert(entry->modifiers);
	memcpy(entry->modifiers, modifiers->data, entry->n_modifiers * 8);
#endif
}

static void session_handle_dmabuf_device(void* data,
		struct ext_image_copy_capture_session_v1* session,
		struct wl_array *device)
{
	struct ext_image_copy_capture* self = data;

	clear_constraints(self);

	if (device->size != sizeof(self->dmabuf_dev)) {
		nvnc_log(NVNC_LOG_ERROR, "array size != sizeof(dev_t)");
		return;
	}

	self->have_dmabuf_dev = true;
	memcpy(&self->dmabuf_dev, device->data, sizeof(self->dmabuf_dev));
}

static void session_handle_dimensions(void *data,
		struct ext_image_copy_capture_session_v1 *session, uint32_t width,
		uint32_t height)
{
	struct ext_image_copy_capture* self = data;

	clear_constraints(self);

	nvnc_log(NVNC_LOG_DEBUG, "Buffer dimensions: %"PRIu32"x%"PRIu32,
			width, height);

	self->width = width;
	self->height = height;
	self->wl_shm_stride = width * 4;
}

static void session_handle_constraints_done(void *data,
		struct ext_image_copy_capture_session_v1 *session)
{
	struct ext_image_copy_capture* self = data;
	struct wv_buffer_config config = {};

	config.width = self->width;
	config.height = self->height;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (self->n_dmabuf_formats && self->parent.enable_linux_dmabuf) {
		// TODO: Select "best" format
		config.format = self->dmabuf_formats[0].format;
		config.stride = 0;
		config.type = WV_BUFFER_DMABUF;

		if (self->have_dmabuf_dev)
			config.node = self->dmabuf_dev;
	} else
#endif
	if (self->n_wl_shm_formats > 0) {
		// TODO: Select "best" format
		config.format = self->wl_shm_formats[0];
		config.stride = self->wl_shm_stride;
		config.type = WV_BUFFER_SHM;
	} else {
		nvnc_log(NVNC_LOG_DEBUG, "No buffer formats supplied");
		return;
	}

	wv_buffer_pool_reconfig(self->pool, &config);

	if (self->should_start) {
		ext_image_copy_capture_schedule_capture(self);
		self->should_start = false;
	}

	self->have_constraints = true;

	nvnc_log(NVNC_LOG_DEBUG, "Init done");
}

static void restart_session(struct ext_image_copy_capture* self)
{
	bool is_cursor_session = self->cursor;
	ext_image_copy_capture_deinit_session(self);
	if (is_cursor_session)
		ext_image_copy_capture_init_cursor_session(self);
	else
		ext_image_copy_capture_init_session(self);
}

static void session_handle_stopped(void* data,
		struct ext_image_copy_capture_session_v1* session)
{
	nvnc_log(NVNC_LOG_DEBUG, "Session %p stopped", session);
	// TODO: Restart session if it is stopped?
}

static void frame_handle_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform)
{
	struct ext_image_copy_capture* self = data;

	assert(self->buffer);

	// TODO: Tell main.c not to override this transform
	nvnc_fb_set_transform(self->buffer->nvnc_fb, transform);
}

static void frame_handle_ready(void *data,
		struct ext_image_copy_capture_frame_v1 *frame)
{
	struct ext_image_copy_capture* self = data;

	assert(frame == self->frame);
	ext_image_copy_capture_frame_v1_destroy(self->frame);
	self->frame = NULL;

	float damage_area = calculate_region_area(&self->buffer->frame_damage);
	float pixel_area = self->buffer->width * self->buffer->height;
	nvnc_trace("Frame ready with damage: %.02f %", 100.0 * damage_area /
			pixel_area);

	assert(self->buffer);

	enum wv_buffer_domain domain = self->cursor ?
		WV_BUFFER_DOMAIN_CURSOR : WV_BUFFER_DOMAIN_OUTPUT;
	wv_buffer_registry_damage_all(&self->buffer->frame_damage, domain);
	pixman_region_clear(&self->buffer->buffer_damage);

	struct wv_buffer* buffer = self->buffer;
	self->buffer = NULL;

	buffer->x_hotspot = self->hotspot.x;
	buffer->y_hotspot = self->hotspot.y;

	self->frame_count++;

	// TODO: Use presentation time somehow?
	self->last_time = gettime_us();

	self->parent.on_done(SCREENCOPY_DONE, buffer, self->parent.userdata);
}

static void frame_handle_failed(void *data,
		struct ext_image_copy_capture_frame_v1 *frame,
		enum ext_image_copy_capture_frame_v1_failure_reason reason)
{
	struct ext_image_copy_capture* self = data;

	assert(frame == self->frame);
	ext_image_copy_capture_frame_v1_destroy(self->frame);
	self->frame = NULL;

	nvnc_log(NVNC_LOG_DEBUG, "Failed!\n");

	assert(self->buffer);

	wv_buffer_pool_release(self->pool, self->buffer);
	self->buffer = NULL;

	if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
		screencopy_start(&self->parent, false);
		return;
	}

	self->parent.on_done(SCREENCOPY_FATAL, NULL, self->parent.userdata);
}

static void frame_handle_damage(void *data,
		struct ext_image_copy_capture_frame_v1 *frame,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct ext_image_copy_capture* self = data;

	nvnc_trace("Got frame damage: %dx%d", width, height);
	wv_buffer_damage_rect(self->buffer, x, y, width, height);
}

static void frame_handle_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *frame,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	struct ext_image_copy_capture* self = data;

	uint64_t sec = (uint64_t)sec_hi << 32 | (uint64_t)sec_lo;
	uint64_t pts = sec * UINT64_C(1000000) + (uint64_t)nsec / UINT64_C(1000);
	nvnc_trace("Setting buffer pts: %" PRIu64, pts);
	nvnc_fb_set_pts(self->buffer->nvnc_fb, pts);
}

static struct ext_image_copy_capture_session_v1_listener session_listener = {
	.shm_format = session_handle_format_shm,
	.dmabuf_format = session_handle_format_drm,
	.dmabuf_device = session_handle_dmabuf_device,
	.buffer_size = session_handle_dimensions,
	.done = session_handle_constraints_done,
	.stopped = session_handle_stopped,
};

static struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.damage = frame_handle_damage,
	.presentation_time = frame_handle_presentation_time,
	.transform = frame_handle_transform,
	.ready = frame_handle_ready,
	.failed = frame_handle_failed,
};

static void cursor_handle_enter(void* data,
		struct ext_image_copy_capture_cursor_session_v1* cursor)
{
	struct ext_image_copy_capture* self = data;
	if (self->parent.cursor_enter)
		self->parent.cursor_enter(self->parent.userdata);
}

static void cursor_handle_leave(void* data,
		struct ext_image_copy_capture_cursor_session_v1* cursor)
{
	struct ext_image_copy_capture* self = data;
	if (self->parent.cursor_leave)
		self->parent.cursor_leave(self->parent.userdata);
}

static void cursor_handle_position(void* data,
		struct ext_image_copy_capture_cursor_session_v1* cursor, int x, int y)
{
	// Don't care
}

static void cursor_handle_hotspot(void* data,
		struct ext_image_copy_capture_cursor_session_v1* cursor, int x, int y)
{
	struct ext_image_copy_capture* self = data;
	self->hotspot.x = x;
	self->hotspot.y = y;

	if (self->parent.cursor_hotspot)
		self->parent.cursor_hotspot(x, y, self->parent.userdata);

	nvnc_trace("Got hotspot at %d, %d", x, y);
}

static struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
	.enter = cursor_handle_enter,
	.leave = cursor_handle_leave,
	.position = cursor_handle_position,
	.hotspot = cursor_handle_hotspot,
};

static int ext_image_copy_capture_start(struct screencopy* ptr, bool immediate)
{
	struct ext_image_copy_capture* self = (struct ext_image_copy_capture*)ptr;
	if (self->frame) {
		return -1;
	}

	if (immediate && self->frame_count != 0) {
		// Flush state:
		restart_session(self);
		self->should_start = true;
		return 0;
	}

	if (!self->have_constraints) {
		self->should_start = true;
		return 0;
	}

	uint64_t eps = 4000; // µs
	uint64_t period = round(1e6 / self->parent.rate_limit);
	uint64_t next_time = self->last_time + period - eps;
	uint64_t now = gettime_us();

	if (now >= next_time) {
		aml_stop(aml_get_default(), self->timer);
		ext_image_copy_capture_schedule_capture(self);
	} else {
		nvnc_trace("Scheduling %scapture after %"PRIu64" µs",
				self->cursor ? "cursor " : "", next_time - now);
		aml_set_duration(self->timer, next_time - now);
		aml_start(aml_get_default(), self->timer);
	}

	return 0;
}

static void ext_image_copy_capture_stop(struct screencopy* base)
{
	struct ext_image_copy_capture* self = (struct ext_image_copy_capture*)base;

	aml_stop(aml_get_default(), self->timer);

	if (self->frame) {
		ext_image_copy_capture_frame_v1_destroy(self->frame);
		self->frame = NULL;
	}
}

static struct screencopy* ext_image_copy_capture_create(struct wl_output* output,
		bool render_cursor)
{
	struct ext_image_copy_capture* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &ext_image_copy_capture_impl;
	self->parent.rate_limit = 30;

	self->wl_output = output;
	self->render_cursors = render_cursor;

	self->timer = aml_timer_new(0, ext_image_copy_capture_schedule_from_timer, self,
			NULL);
	assert(self->timer);

	self->pool = wv_buffer_pool_create(NULL);
	if (!self->pool)
		goto failure;

	if (ext_image_copy_capture_init_session(self) < 0)
		goto session_failure;

	return (struct screencopy*)self;

session_failure:
	wv_buffer_pool_destroy(self->pool);
failure:
	free(self);
	return NULL;
}

static struct screencopy* ext_image_copy_capture_create_cursor(struct wl_output* output,
		struct wl_seat* seat)
{
	struct ext_image_copy_capture* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &ext_image_copy_capture_impl;
	self->parent.rate_limit = 30;

	self->wl_output = output;
	self->wl_seat = seat;

	self->timer = aml_timer_new(0, ext_image_copy_capture_schedule_from_timer, self,
			NULL);
	assert(self->timer);

	self->pool = wv_buffer_pool_create(NULL);
	if (!self->pool)
		goto failure;

	if (ext_image_copy_capture_init_cursor_session(self) < 0)
		goto session_failure;

	return (struct screencopy*)self;

session_failure:
	wv_buffer_pool_destroy(self->pool);
failure:
	free(self);
	return NULL;
}

void ext_image_copy_capture_destroy(struct screencopy* ptr)
{
	struct ext_image_copy_capture* self = (struct ext_image_copy_capture*)ptr;

	aml_stop(aml_get_default(), self->timer);
	aml_unref(self->timer);

	ext_image_copy_capture_deinit_session(self);

	wv_buffer_pool_destroy(self->pool);

	clear_dmabuf_formats(self);
	free(self->dmabuf_formats);
	free(self->wl_shm_formats);
	free(self);
}

struct screencopy_impl ext_image_copy_capture_impl = {
	.caps = SCREENCOPY_CAP_CURSOR | SCREENCOPY_CAP_TRANSFORM,
	.create = ext_image_copy_capture_create,
	.create_cursor = ext_image_copy_capture_create_cursor,
	.destroy = ext_image_copy_capture_destroy,
	.start = ext_image_copy_capture_start,
	.stop = ext_image_copy_capture_stop,
};
