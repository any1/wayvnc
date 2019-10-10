#pragma once

#include <stdbool.h>
#include "wlr-screencopy-unstable-v1.h"
#include "frame-capture.h"

struct zwlr_screencopy_manager_v1;
struct zwlr_screencopy_frame_v1;
struct wl_output;
struct wl_buffer;
struct wl_shm;

enum screencopy_status {
	SCREENCOPY_STATUS_CAPTURING = 0,
	SCREENCOPY_STATUS_FATAL,
	SCREENCOPY_STATUS_FAILED,
	SCREENCOPY_STATUS_DONE,
};

struct screencopy {
	struct frame_capture frame_capture;

	struct wl_shm* wl_shm;
	struct wl_buffer* buffer;

	void* pixels;
	size_t bufsize;

	struct zwlr_screencopy_manager_v1* manager;
	struct zwlr_screencopy_frame_v1* frame;
};

void screencopy_init(struct screencopy* self);
