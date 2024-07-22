/*
 * Copyright (c) 2020 - 2024 Andri Yngvason
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

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/mman.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#include <pixman.h>
#include <neatvnc.h>

#include "linux-dmabuf-unstable-v1.h"
#include "shm.h"
#include "sys/queue.h"
#include "buffer.h"
#include "pixels.h"
#include "config.h"
#include "util.h"

#ifdef ENABLE_SCREENCOPY_DMABUF
#include <gbm.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#ifdef HAVE_LINUX_DMA_HEAP
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#define LINUX_CMA_PATH "/dev/dma_heap/linux,cma"
#endif // HAVE_LINUX_DMA_HEAP
#endif // ENABLE_SCREENCOPY_DMABUF

extern struct wl_shm* wl_shm;
extern struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf;
extern struct gbm_device* gbm_device;

enum wv_buffer_type wv_buffer_get_available_types(void)
{
	enum wv_buffer_type type = 0;

	if (wl_shm)
		type |= WV_BUFFER_SHM;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (zwp_linux_dmabuf && gbm_device)
		type |= WV_BUFFER_DMABUF;
#endif

	return type;
}

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

	int bpp = pixel_size_from_fourcc(fourcc);
	assert(bpp > 0);
	self->nvnc_fb = nvnc_fb_from_buffer(self->pixels, width, height, fourcc,
			stride / bpp);
	if (!self->nvnc_fb) {
		goto nvnc_fb_failure;
	}

	nvnc_set_userdata(self->nvnc_fb, self, NULL);

	pixman_region_init(&self->damage);

	close(fd);
	return self;

nvnc_fb_failure:
	wl_buffer_destroy(self->wl_buffer);
shm_failure:
pool_failure:
mmap_failure:
	close(fd);
failure:
	free(self);
	return NULL;
}

#ifdef ENABLE_SCREENCOPY_DMABUF
#ifdef HAVE_LINUX_DMA_HEAP
static bool have_linux_cma(void)
{
	return access(LINUX_CMA_PATH, R_OK | W_OK) == 0;
}

static int linux_cma_alloc(size_t size)
{
	int fd = open(LINUX_CMA_PATH, O_RDWR | O_CLOEXEC, 0);
	if (fd < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to open CMA device: %m");
		return -1;
	}

	struct dma_heap_allocation_data data = {
		.len = size,
		.fd_flags = O_CLOEXEC | O_RDWR,
	};

	int r = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &data);
	if (r < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to allocate CMA buffer: %m");
		return -1;
	}
	close(fd);

        return data.fd;
}

// Some devices (mostly ARM SBCs) need CMA for hardware encoders.
static struct gbm_bo* create_cma_gbm_bo(int width, int height, uint32_t fourcc)
{
	assert(gbm_device);

	int bpp = pixel_size_from_fourcc(fourcc);
	if (!bpp) {
		nvnc_log(NVNC_LOG_PANIC, "Unsupported pixel format: %" PRIu32,
				fourcc);
	}

	/* TODO: Get alignment through feedback mechanism.
	 * Buffer sizes are aligned on both axes by 16 and we'll do the same
	 * in the encoder, but this requirement should come from the encoder.
	 */
	int stride = bpp * ALIGN_UP(width, 16);

	int fd = linux_cma_alloc(stride * ALIGN_UP(height, 16));
	if (fd < 0) {
		return NULL;
	}

	struct gbm_import_fd_modifier_data d = {
		.format = fourcc,
		.width = width,
		.height = height,
		// v4l2m2m doesn't support modifiers, so we use linear
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.num_fds = 1,
		.fds[0] = fd,
		.offsets[0] = 0,
		.strides[0] = stride,
	};

	struct gbm_bo* bo = gbm_bo_import(gbm_device, GBM_BO_IMPORT_FD_MODIFIER,
			&d, 0);
	if (!bo) {
		nvnc_log(NVNC_LOG_DEBUG, "Failed to import dmabuf: %m");
		close(fd);
		return NULL;
	}

	return bo;
}
#endif // HAVE_LINUX_DMA_HEAP

static struct wv_buffer* wv_buffer_create_dmabuf(int width, int height,
		uint32_t fourcc)
{
	assert(zwp_linux_dmabuf);
	assert(gbm_device);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = WV_BUFFER_DMABUF;
	self->width = width;
	self->height = height;
	self->format = fourcc;

#ifdef HAVE_LINUX_DMA_HEAP
	self->bo = have_linux_cma() ?
		create_cma_gbm_bo(width, height, fourcc) :
		gbm_bo_create(gbm_device, width, height, fourcc,
				GBM_BO_USE_RENDERING);
#else
	self->bo = gbm_bo_create(gbm_device, width, height, fourcc,
				GBM_BO_USE_RENDERING);
#endif

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
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);

	if (!self->wl_buffer)
		goto buffer_failure;

	self->nvnc_fb = nvnc_fb_from_gbm_bo(self->bo);
	if (!self->nvnc_fb) {
		goto nvnc_fb_failure;
	}

	nvnc_set_userdata(self->nvnc_fb, self, NULL);

	return self;

nvnc_fb_failure:
	wl_buffer_destroy(self->wl_buffer);
buffer_failure:
fd_failure:
	zwp_linux_buffer_params_v1_destroy(params);
params_failure:
	gbm_bo_destroy(self->bo);
bo_failure:
	free(self);
	return NULL;
}
#endif

struct wv_buffer* wv_buffer_create(enum wv_buffer_type type, int width,
		int height, int stride, uint32_t fourcc)
{
	switch (type) {
	case WV_BUFFER_SHM:
		return wv_buffer_create_shm(width, height, stride, fourcc);
#ifdef ENABLE_SCREENCOPY_DMABUF
	case WV_BUFFER_DMABUF:
		return wv_buffer_create_dmabuf(width, height, fourcc);
#endif
	case WV_BUFFER_UNSPEC:;
	}

	abort();
	return NULL;
}

static void wv_buffer_destroy_shm(struct wv_buffer* self)
{
	nvnc_fb_unref(self->nvnc_fb);
	wl_buffer_destroy(self->wl_buffer);
	munmap(self->pixels, self->size);
	free(self);
}

#ifdef ENABLE_SCREENCOPY_DMABUF
static void wv_buffer_destroy_dmabuf(struct wv_buffer* self)
{
	nvnc_fb_unref(self->nvnc_fb);
	wl_buffer_destroy(self->wl_buffer);
	gbm_bo_destroy(self->bo);
	free(self);
}
#endif

void wv_buffer_destroy(struct wv_buffer* self)
{
	pixman_region_fini(&self->damage);

	switch (self->type) {
	case WV_BUFFER_SHM:
		wv_buffer_destroy_shm(self);
		return;
#ifdef ENABLE_SCREENCOPY_DMABUF
	case WV_BUFFER_DMABUF:
		wv_buffer_destroy_dmabuf(self);
		return;
#endif
	case WV_BUFFER_UNSPEC:;
	}

	abort();
}

void wv_buffer_damage_rect(struct wv_buffer* self, int x, int y, int width,
		int height)
{
	pixman_region_union_rect(&self->damage, &self->damage, x, y, width,
			height);
}

void wv_buffer_damage_whole(struct wv_buffer* self)
{
	wv_buffer_damage_rect(self, 0, 0, self->width, self->height);
}

void wv_buffer_damage_clear(struct wv_buffer* self)
{
	pixman_region_clear(&self->damage);
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

static bool wv_buffer_pool_match_buffer(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer)
{
	if (pool->type != buffer->type)
		return false;

	switch (pool->type) {
	case WV_BUFFER_SHM:
		if (pool->stride != buffer->stride) {
			return false;
		}

#ifdef ENABLE_SCREENCOPY_DMABUF
		/* fall-through */
	case WV_BUFFER_DMABUF:
#endif
		if (pool->width != buffer->width
		    || pool->height != buffer->height
		    || pool->format != buffer->format)
			return false;

		return true;
	case WV_BUFFER_UNSPEC:
		abort();
	}

	return false;
}

void wv_buffer_pool__on_release(struct nvnc_fb* fb, void* context)
{
	struct wv_buffer* buffer = nvnc_get_userdata(fb);
	struct wv_buffer_pool* pool = context;

	wv_buffer_pool_release(pool, buffer);
}

struct wv_buffer* wv_buffer_pool_acquire(struct wv_buffer_pool* pool)
{
	struct wv_buffer* buffer = TAILQ_FIRST(&pool->queue);
	if (buffer) {
		assert(wv_buffer_pool_match_buffer(pool, buffer));
		TAILQ_REMOVE(&pool->queue, buffer, link);
		return buffer;
	}

	buffer = wv_buffer_create(pool->type, pool->width, pool->height,
			pool->stride, pool->format);
	if (buffer)
		nvnc_fb_set_release_fn(buffer->nvnc_fb,
				wv_buffer_pool__on_release, pool);

	return buffer;
}

void wv_buffer_pool_release(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer)
{
	wv_buffer_damage_clear(buffer);

	if (wv_buffer_pool_match_buffer(pool, buffer)) {
		TAILQ_INSERT_TAIL(&pool->queue, buffer, link);
	} else {
		wv_buffer_destroy(buffer);
	}
}
