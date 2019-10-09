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

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct zwlr_export_dmabuf_manager_v1;
struct zwlr_export_dmabuf_frame_v1;
struct wl_output;

enum dmabuf_capture_status {
        DMABUF_CAPTURE_UNSPEC = 0,
        DMABUF_CAPTURE_IN_PROGRESS,
        DMABUF_CAPTURE_CANCELLED,
        DMABUF_CAPTURE_FATAL,
        DMABUF_CAPTURE_DONE
};

struct dmabuf_plane {
	int fd;
	uint32_t offset;
	uint32_t size;
	uint32_t pitch;
	uint64_t modifier;
};

struct dmabuf_frame {
	uint32_t width;
	uint32_t height;
	uint32_t format;

	uint32_t n_planes;
	struct dmabuf_plane plane[4];
};

struct dmabuf_capture {
	struct zwlr_export_dmabuf_manager_v1* manager;
	struct zwlr_export_dmabuf_frame_v1* zwlr_frame;
	struct dmabuf_frame frame;

        bool overlay_cursor;
        struct wl_output* output;
        enum dmabuf_capture_status status;
        void (*on_done)(struct dmabuf_capture*);

        void* userdata;
};

int dmabuf_capture_start(struct dmabuf_capture* self);
void dmabuf_capture_stop(struct dmabuf_capture* self);
