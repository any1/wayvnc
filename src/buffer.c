#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/mman.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#include <gbm.h>

#include "linux-dmabuf-unstable-v1.h"
#include "shm.h"
#include "sys/queue.h"
#include "buffer.h"
#include "pixels.h"

extern struct wl_shm* wl_shm;
extern struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf;
extern struct gbm_device* gbm_device;

struct wv_buffer* wv_buffer_create_shm(int width,
		int height, int stride, uint32_t fourcc)
{
	assert(wl_shm);
	enum wl_shm_format wl_fmt = fourcc_to_wl_shm(fourcc);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = WV_BUFFER_SHM;
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

static struct wv_buffer* wv_buffer_create_dmabuf(int width, int height,
		uint32_t fourcc)
{
	assert(zwp_linux_dmabuf);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = WV_BUFFER_DMABUF;
	self->width = width;
	self->height = height;
	self->format = fourcc;

	self->bo = gbm_bo_create(gbm_device, width, height, fourcc,
			GBM_BO_USE_RENDERING);
	if (!self->bo)
		goto bo_failure;

	struct zwp_linux_buffer_params_v1* params;
	params = zwp_linux_dmabuf_v1_create_params(zwp_linux_dmabuf);
	if (!params)
		goto params_failure;

	uint32_t offset = gbm_bo_get_offset(self->bo, 0);
	uint32_t stride = gbm_bo_get_stride(self->bo);
	uint64_t mod = gbm_bo_get_modifier(self->bo);
	int fd = gbm_bo_get_fd(self->bo);
	if (fd < 0)
		goto fd_failure;

	zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
			mod >> 32, mod & 0xffffffff);
	self->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, width,
			height, fourcc, /* flags */ 0);
	close(fd); // TODO: Maybe keep this open?

	if (!self->wl_buffer)
		goto buffer_failure;

	return self;

buffer_failure:
fd_failure:
params_failure:
	gbm_bo_destroy(self->bo);
bo_failure:
	free(self);
	return NULL;
}

struct wv_buffer* wv_buffer_create(enum wv_buffer_type type, int width,
		int height, int stride, uint32_t fourcc)
{
	switch (type) {
	case WV_BUFFER_SHM:
		return wv_buffer_create_shm(width, height, stride, fourcc);
	case WV_BUFFER_DMABUF:
		return wv_buffer_create_dmabuf(width, height, fourcc);
	case WV_BUFFER_UNSPEC:;
	}

	abort();
	return NULL;
}

static void wv_buffer_destroy_shm(struct wv_buffer* self)
{
	wl_buffer_destroy(self->wl_buffer);
	munmap(self->pixels, self->size);
	free(self);
}

static void wv_buffer_destroy_dmabuf(struct wv_buffer* self)
{
	wl_buffer_destroy(self->wl_buffer);
	gbm_bo_destroy(self->bo);
	free(self);
}

void wv_buffer_destroy(struct wv_buffer* self)
{
	wv_buffer_unmap(self);

	switch (self->type) {
	case WV_BUFFER_SHM:
		wv_buffer_destroy_shm(self);
		return;
	case WV_BUFFER_DMABUF:
		wv_buffer_destroy_dmabuf(self);
		return;
	case WV_BUFFER_UNSPEC:;
	}

	abort();
}

static int wv_buffer_map_dmabuf(struct wv_buffer* self)
{
	if (self->bo_map_handle)
		return 0;

	uint32_t stride = 0;
	self->pixels = gbm_bo_map(self->bo, 0, 0, self->width, self->height,
			GBM_BO_TRANSFER_READ, &stride, &self->bo_map_handle);
	self->stride = stride;
	if (self->pixels)
		return 0;

	self->bo_map_handle = NULL;
	return -1;
}

int wv_buffer_map(struct wv_buffer* self)
{
	switch (self->type) {
	case WV_BUFFER_SHM:
		return 0;
	case WV_BUFFER_DMABUF:
		return wv_buffer_map_dmabuf(self);
	case WV_BUFFER_UNSPEC:;
	}

	abort();
}

static void wv_buffer_unmap_dmabuf(struct wv_buffer* self)
{
	if (self->bo_map_handle)
		gbm_bo_unmap(self->bo, self->bo_map_handle);
	self->bo_map_handle = NULL;
}

void wv_buffer_unmap(struct wv_buffer* self)
{
	switch (self->type) {
	case WV_BUFFER_SHM:
		return;
	case WV_BUFFER_DMABUF:
		return wv_buffer_unmap_dmabuf(self);
	case WV_BUFFER_UNSPEC:;

	}

	abort();
}

struct wv_buffer_pool* wv_buffer_pool_create(enum wv_buffer_type type,
		int width, int height, int stride, uint32_t format)
{
	struct wv_buffer_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	TAILQ_INIT(&self->queue);
	self->type = type;
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
		enum wv_buffer_type type, int width, int height, int stride,
		uint32_t format)
{
	if (pool->type != type || pool->width != width || pool->height != height
	    || pool->stride != stride || pool->format != format) {
		wv_buffer_pool_clear(pool);
	}

	pool->type = type;
	pool->width = width;
	pool->height = height;
	pool->stride = stride;
	pool->format = format;
}

struct wv_buffer* wv_buffer_pool_acquire(struct wv_buffer_pool* pool)
{
	struct wv_buffer* buffer = TAILQ_FIRST(&pool->queue);
	if (buffer) {
		assert(pool->type == buffer->type
		       && pool->width == buffer->width
		       && pool->height == buffer->height
		       && pool->stride == buffer->stride
		       && pool->format == buffer->format);

		TAILQ_REMOVE(&pool->queue, buffer, link);
		return buffer;
	}

	return wv_buffer_create(pool->type, pool->width, pool->height,
			pool->stride, pool->format);
}

void wv_buffer_pool_release(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer)
{
	wv_buffer_unmap(buffer);

	if (pool->width == buffer->width
	    && pool->height == buffer->height
	    && pool->stride == buffer->stride
	    && pool->format == buffer->format) {
		TAILQ_INSERT_TAIL(&pool->queue, buffer, link);
	} else {
		wv_buffer_destroy(buffer);
	}
}
