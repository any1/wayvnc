#pragma once

#include "sys/queue.h"

#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

struct wl_buffer;
struct gbm_bo;

enum wv_buffer_type {
	WV_BUFFER_UNSPEC = 0,
	WV_BUFFER_SHM,
	WV_BUFFER_DMABUF,
};

struct wv_buffer {
	enum wv_buffer_type type;
	TAILQ_ENTRY(wv_buffer) link;

	struct wl_buffer* wl_buffer;

	void* pixels;
	size_t size;
	int width, height, stride;
	uint32_t format;
	bool y_inverted;

	/* The following is only applicable to DMABUF */
	struct gbm_bo* bo;
	void* bo_map_handle;

	bool is_ready;
	void (*on_ready)(struct wv_buffer*);
	void* userdata;
};

TAILQ_HEAD(wv_buffer_queue, wv_buffer);

struct wv_buffer_pool {
	struct wv_buffer_queue queue;
	enum wv_buffer_type type;
	int width, height, stride;
	uint32_t format;
};

struct wv_buffer* wv_buffer_create(enum wv_buffer_type, int width, int height,
		int stride, uint32_t fourcc);
void wv_buffer_destroy(struct wv_buffer* self);

int wv_buffer_map(struct wv_buffer* self);
void wv_buffer_unmap(struct wv_buffer* self);

struct wv_buffer_pool* wv_buffer_pool_create(enum wv_buffer_type, int width,
		int height, int stride, uint32_t format);
void wv_buffer_pool_destroy(struct wv_buffer_pool* pool);
void wv_buffer_pool_resize(struct wv_buffer_pool* pool, enum wv_buffer_type,
		int width, int height, int stride, uint32_t format);
struct wv_buffer* wv_buffer_pool_acquire(struct wv_buffer_pool* pool);
void wv_buffer_pool_release(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer);
