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

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct wl_output;
struct nvnc_fb;
struct renderer;

enum frame_capture_status {
	CAPTURE_STOPPED = 0,
	CAPTURE_IN_PROGRESS,
	CAPTURE_FAILED,
	CAPTURE_FATAL,
	CAPTURE_DONE,
};

struct frame_capture {
	enum frame_capture_status status;

	bool overlay_cursor;
	struct wl_output* wl_output;

	void* userdata;
	void (*on_done)(struct frame_capture*);

	struct {
		uint32_t fourcc_format;
		uint32_t width;
		uint32_t height;
		uint32_t stride;
	} frame_info;

	struct {
		uint32_t x;
		uint32_t y;
		uint32_t width;
		uint32_t height;
	} damage_hint;

	struct {
		void (*render)(struct frame_capture*, struct renderer*,
		               struct nvnc_fb* fb);
		int (*start)(struct frame_capture*);
		void (*stop)(struct frame_capture*);
	} backend;
};

static inline int frame_capture_start(struct frame_capture* self)
{
	return self->backend.start(self);
}

static inline void frame_capture_stop(struct frame_capture* self)
{
	self->backend.stop(self);
}

static inline void frame_capture_render(struct frame_capture* self,
                                        struct renderer* renderer,
                                        struct nvnc_fb* fb)
{
	return self->backend.render(self, renderer, fb);
}
