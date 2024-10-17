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

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wl_output;
struct wl_seat;
struct wv_buffer;

enum screencopy_result {
	SCREENCOPY_DONE,
	SCREENCOPY_FATAL,
	SCREENCOPY_FAILED,
};

enum screencopy_capabilitites {
	SCREENCOPY_CAP_CURSOR = 1 << 0,
	SCREENCOPY_CAP_TRANSFORM = 1 << 1,
};

struct screencopy_dmabuf_format {
	uint32_t format;
	uint64_t modifier;
};

typedef void (*screencopy_done_fn)(enum screencopy_result,
		struct wv_buffer* buffer, void* userdata);

struct screencopy_impl {
	enum screencopy_capabilitites caps;
	struct screencopy* (*create)(struct wl_output*, bool render_cursor);
	struct screencopy* (*create_cursor)(struct wl_output*, struct wl_seat*);
	void (*destroy)(struct screencopy*);
	int (*start)(struct screencopy*, bool immediate);
	void (*stop)(struct screencopy*);
};

struct screencopy {
	struct screencopy_impl* impl;

	double rate_limit;
	bool enable_linux_dmabuf;

	screencopy_done_fn on_done;
	void (*cursor_enter)(void* userdata);
	void (*cursor_leave)(void* userdata);
	void (*cursor_hotspot)(int x, int y, void* userdata);

	int (*select_format)(void* userdata, const uint32_t* formats,
			int n_formats);
	int (*select_dmabuf_format)(void* userdata,
			const struct screencopy_dmabuf_format* formats,
			int n_formats);

	void* userdata;
};

struct screencopy* screencopy_create(struct wl_output* output,
		bool render_cursor);
struct screencopy* screencopy_create_cursor(struct wl_output* output,
		struct wl_seat* seat);
void screencopy_destroy(struct screencopy* self);

int screencopy_start(struct screencopy* self, bool immediate);
void screencopy_stop(struct screencopy* self);
