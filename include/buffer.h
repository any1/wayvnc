#pragma once

#include "sys/queue.h"

#include <unistd.h>
#include <pixman.h>
#include <stdbool.h>

struct wl_buffer;

struct wv_buffer {
	TAILQ_ENTRY(wv_buffer) link;
	struct wl_buffer* wl_buffer;
	void* pixels;
	size_t size;
	pixman_image_t* image;
	int width, height, stride;
	uint32_t format;
	bool y_inverted;
};

TAILQ_HEAD(wv_buffer_queue, wv_buffer);

struct wv_buffer_pool {
	struct wv_buffer_queue queue;
	int width, height, stride;
	uint32_t format;
};

struct wv_buffer* wv_buffer_create(int width, int height, int stride,
		uint32_t fourcc);
void wv_buffer_destroy(struct wv_buffer* self);

struct wv_buffer_pool* wv_buffer_pool_create(int width, int height, int stride,
		uint32_t format);
void wv_buffer_pool_destroy(struct wv_buffer_pool* pool);
void wv_buffer_pool_resize(struct wv_buffer_pool* pool,
		int width, int height, int stride, uint32_t format);
struct wv_buffer* wv_buffer_pool_acquire(struct wv_buffer_pool* pool);
void wv_buffer_pool_release(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer);
