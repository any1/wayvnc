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

#include <stdbool.h>

#include "wlr-screencopy-unstable-v1.h"
#include "smooth.h"
#include "buffer.h"

struct zwlr_screencopy_manager_v1;
struct zwlr_screencopy_frame_v1;
struct wl_output;
struct wl_buffer;
struct wl_shm;
struct aml_timer;
struct renderer;

enum screencopy_status {
	SCREENCOPY_STOPPED = 0,
	SCREENCOPY_IN_PROGRESS,
	SCREENCOPY_FAILED,
	SCREENCOPY_FATAL,
	SCREENCOPY_DONE,
};

struct screencopy {
	enum screencopy_status status;

	struct wv_buffer_pool* pool;
	struct wv_buffer* front;
	struct wv_buffer* back;

	struct zwlr_screencopy_manager_v1* manager;
	struct zwlr_screencopy_frame_v1* frame;
	int version;

	void* userdata;
	void (*on_done)(struct screencopy*);

	uint64_t last_time;
	uint64_t start_time;
	struct aml_timer* timer;

	struct smooth delay_smoother;
	double delay;
	bool is_immediate_copy;
	bool overlay_cursor;
	struct wl_output* wl_output;

	uint32_t wl_shm_width, wl_shm_height, wl_shm_stride;
	enum wl_shm_format wl_shm_format;

	bool have_linux_dmabuf;
	uint32_t dmabuf_width, dmabuf_height;
	uint32_t fourcc;
};

void screencopy_init(struct screencopy* self);
void screencopy_destroy(struct screencopy* self);

int screencopy_start(struct screencopy* self);
int screencopy_start_immediate(struct screencopy* self);

void screencopy_stop(struct screencopy* self);
