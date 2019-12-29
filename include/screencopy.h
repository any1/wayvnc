#pragma once

#include <stdbool.h>
#include <uv.h>

#include "wlr-screencopy-unstable-v1.h"
#include "frame-capture.h"
#include "smooth.h"

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

	uint64_t last_time;

	struct smooth rate_filter;
	double rate;
	uv_timer_t timer;
	bool is_rate_limited;
};

void screencopy_init(struct screencopy* self);
void screencopy_destroy(struct screencopy* self);
