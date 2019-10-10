#pragma once

#include <stdbool.h>
#include "wlr-screencopy-unstable-v1.h"

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
	struct wl_shm* wl_shm;
	struct wl_buffer* buffer;

	void* pixels;
	size_t bufsize;

	struct zwlr_screencopy_manager_v1* manager;
	struct zwlr_screencopy_frame_v1* frame;

	bool overlay_cursor;
	struct wl_output* output;
	enum screencopy_status status;
	void (*on_done)(struct screencopy*);

	enum wl_shm_format format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;

	void* userdata;
};

int screencopy_start(struct screencopy* self);
void screencopy_stop(struct screencopy* self);
