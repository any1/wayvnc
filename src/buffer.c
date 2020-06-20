#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/mman.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>

#include "shm.h"
#include "sys/queue.h"
#include "buffer.h"
#include "pixels.h"

extern struct wl_shm* wl_shm;

struct wv_buffer* wv_buffer_create(int width, int height, int stride,
		uint32_t fourcc)
{
	assert(wl_shm);
	enum wl_shm_format wl_fmt = fourcc_to_wl_shm(fourcc);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = fourcc;

	self->size = height * stride;
	int fd = shm_alloc_fd(self->size);
	if (fd < 0)
		goto failure;

	self->pixels = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (!self->pixels)
		goto mmap_failure;

	struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, self->size);
	if (!pool)
		goto pool_failure;

	self->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
			stride, wl_fmt);
	wl_shm_pool_destroy(pool);
	if (!self->wl_buffer)
		goto shm_failure;

	close(fd);
	return self;

shm_failure:
pool_failure:
	munmap(self->pixels, self->size);
mmap_failure:
	close(fd);
failure:
	free(self);
	return NULL;
}

void wv_buffer_destroy(struct wv_buffer* self)
{
	wl_buffer_destroy(self->wl_buffer);
	munmap(self->pixels, self->size);
	free(self);
}

struct wv_buffer_pool* wv_buffer_pool_create(int width, int height, int stride,
		uint32_t format)
{
	struct wv_buffer_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	TAILQ_INIT(&self->queue);
	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = format;

	return self;
}

static void wv_buffer_pool_clear(struct wv_buffer_pool* pool)
{
	while (!TAILQ_EMPTY(&pool->queue)) {
		struct wv_buffer* buffer = TAILQ_FIRST(&pool->queue);
		TAILQ_REMOVE(&pool->queue, buffer, link);
		wv_buffer_destroy(buffer);
	}
}

void wv_buffer_pool_destroy(struct wv_buffer_pool* pool)
{
	wv_buffer_pool_clear(pool);
	free(pool);
}

void wv_buffer_pool_resize(struct wv_buffer_pool* pool,
		int width, int height, int stride, uint32_t format)
{
	if (pool->width != width || pool->height != height
	    || pool->stride != stride || pool->format != format) {
		wv_buffer_pool_clear(pool);
	}

	pool->width = width;
	pool->height = height;
	pool->stride = stride;
	pool->format = format;
}

struct wv_buffer* wv_buffer_pool_acquire(struct wv_buffer_pool* pool)
{
	struct wv_buffer* buffer = TAILQ_FIRST(&pool->queue);
	if (buffer) {
		assert(pool->width == buffer->width
		       && pool->height == buffer->height
		       && pool->stride == buffer->stride
		       && pool->format == buffer->format);

		TAILQ_REMOVE(&pool->queue, buffer, link);
		return buffer;
	}

	return wv_buffer_create(pool->width, pool->height, pool->stride,
			pool->format);
}

void wv_buffer_pool_release(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer)
{
	if (pool->width == buffer->width
	    && pool->height == buffer->height
	    && pool->stride == buffer->stride
	    && pool->format == buffer->format) {
		TAILQ_INSERT_TAIL(&pool->queue, buffer, link);
	} else {
		wv_buffer_destroy(buffer);
	}
}
