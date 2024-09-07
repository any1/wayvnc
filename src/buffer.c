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
#include <sys/stat.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#include <pixman.h>
#include <string.h>
#include <neatvnc.h>

#include "linux-dmabuf-unstable-v1.h"
#include "shm.h"
#include "sys/queue.h"
#include "buffer.h"
#include "pixels.h"
#include "config.h"
#include "util.h"
#include "strlcpy.h"

#ifdef ENABLE_SCREENCOPY_DMABUF
#include <gbm.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>

#ifdef HAVE_LINUX_DMA_HEAP
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#define LINUX_CMA_PATH "/dev/dma_heap/linux,cma"
#endif // HAVE_LINUX_DMA_HEAP
#endif // ENABLE_SCREENCOPY_DMABUF

extern struct wl_shm* wl_shm;
extern struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf;

LIST_HEAD(wv_buffer_list, wv_buffer);

static struct wv_buffer_list buffer_registry;

static bool modifiers_match(const uint64_t* a, int a_len, const uint64_t* b,
		int b_len)
{
	if (a_len != b_len)
		return false;
	return a_len == 0 || memcmp(a, b, a_len) == 0;
}

static bool buffer_configs_match(const struct wv_buffer_config* a,
		const struct wv_buffer_config* b)
{
#define X(n) if (a->n != b->n) return false
	X(type);
	X(width);
	X(height);
	X(stride);
	X(format);
	X(node);
	X(n_modifiers);
#undef X
	return modifiers_match(a->modifiers, a->n_modifiers, b->modifiers,
			b->n_modifiers);
}

static void copy_buffer_config(struct wv_buffer_config* dst,
		const struct wv_buffer_config* src)
{
	free(dst->modifiers);

	memcpy(dst, src, sizeof(*dst));
	dst->n_modifiers = 0;
	dst->modifiers = NULL;

	if (src->n_modifiers > 0) {
		assert(src->modifiers);
		dst->modifiers = malloc(src->n_modifiers * 8);
		assert(dst->modifiers);
		memcpy(dst->modifiers, src->modifiers, src->n_modifiers * 8);
	}
}

enum wv_buffer_type wv_buffer_get_available_types(void)
{
	enum wv_buffer_type type = 0;

	if (wl_shm)
		type |= WV_BUFFER_SHM;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (zwp_linux_dmabuf)
		type |= WV_BUFFER_DMABUF;
#endif

	return type;
}

struct wv_buffer* wv_buffer_create_shm(const struct wv_buffer_config* config)
{
	assert(wl_shm);
	enum wl_shm_format wl_fmt = fourcc_to_wl_shm(config->format);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = WV_BUFFER_SHM;
	self->width = config->width;
	self->height = config->height;
	self->stride = config->stride;
	self->format = config->format;

	self->size = config->height * config->stride;
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

	self->wl_buffer = wl_shm_pool_create_buffer(pool, 0, config->width,
			config->height, config->stride, wl_fmt);
	wl_shm_pool_destroy(pool);
	if (!self->wl_buffer)
		goto shm_failure;

	int bpp = pixel_size_from_fourcc(config->format);
	assert(bpp > 0);
	self->nvnc_fb = nvnc_fb_from_buffer(self->pixels, config->width,
			config->height, config->format, config->stride / bpp);
	if (!self->nvnc_fb) {
		goto nvnc_fb_failure;
	}

	nvnc_set_userdata(self->nvnc_fb, self, NULL);

	pixman_region_init(&self->frame_damage);
	pixman_region_init_rect(&self->buffer_damage, 0, 0, config->width,
			config->height);

	LIST_INSERT_HEAD(&buffer_registry, self, registry_link);

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
static struct gbm_bo* create_cma_gbm_bo(int width, int height, uint32_t fourcc,
		struct gbm_device* gbm)
{
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

	struct gbm_bo* bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &d, 0);
	if (!bo) {
		nvnc_log(NVNC_LOG_DEBUG, "Failed to import dmabuf: %m");
		close(fd);
		return NULL;
	}

	return bo;
}
#endif // HAVE_LINUX_DMA_HEAP

static struct wv_buffer* wv_buffer_create_dmabuf(
		const struct wv_buffer_config* config, struct gbm_device* gbm)
{
	assert(zwp_linux_dmabuf);

	struct wv_buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = WV_BUFFER_DMABUF;
	self->width = config->width;
	self->height = config->height;
	self->format = config->format;
	self->node = config->node;
	self->n_modifiers = config->n_modifiers;

	if (self->n_modifiers > 0) {
		self->modifiers = malloc(config->n_modifiers * 8);
		assert(self->modifiers);
		memcpy(self->modifiers, config->modifiers, self->n_modifiers * 8);
	}

#ifdef HAVE_LINUX_DMA_HEAP
	self->bo = have_linux_cma() ?
		create_cma_gbm_bo(config->width, config->height,
				config->format, gbm) :
		gbm_bo_create_with_modifiers2(gbm, config->width,
				config->height, config->format,
				config->modifiers, config->n_modifiers,
				GBM_BO_USE_RENDERING);
#else
	self->bo = gbm_bo_create_with_modifiers2(gbm, config->width,
			config->height, config->format, config->modifiers,
			config->n_modifiers, GBM_BO_USE_RENDERING);
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
	self->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params,
			config->width, config->height, config->format,
			/* flags */ 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);

	if (!self->wl_buffer)
		goto buffer_failure;

	self->nvnc_fb = nvnc_fb_from_gbm_bo(self->bo);
	if (!self->nvnc_fb) {
		goto nvnc_fb_failure;
	}

	nvnc_set_userdata(self->nvnc_fb, self, NULL);

	pixman_region_init(&self->frame_damage);
	pixman_region_init_rect(&self->buffer_damage, 0, 0, config->width,
			config->height);

	LIST_INSERT_HEAD(&buffer_registry, self, registry_link);

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

struct wv_buffer* wv_buffer_create(const struct wv_buffer_config* config,
		struct gbm_device* gbm)
{
	nvnc_trace("wv_buffer_create: %dx%d, stride: %d, format: %"PRIu32" gbm: %p",
			config->width, config->height, config->stride,
			config->format, gbm);

	switch (config->type) {
	case WV_BUFFER_SHM:
		return wv_buffer_create_shm(config);
#ifdef ENABLE_SCREENCOPY_DMABUF
	case WV_BUFFER_DMABUF:
		return wv_buffer_create_dmabuf(config, gbm);
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
	free(self->modifiers);
	gbm_bo_destroy(self->bo);
	free(self);
}
#endif

void wv_buffer_destroy(struct wv_buffer* self)
{
	pixman_region_fini(&self->buffer_damage);
	pixman_region_fini(&self->frame_damage);
	LIST_REMOVE(self, registry_link);

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
	pixman_region_union_rect(&self->frame_damage, &self->frame_damage, x, y,
			width, height);
}

void wv_buffer_damage_whole(struct wv_buffer* self)
{
	wv_buffer_damage_rect(self, 0, 0, self->width, self->height);
}

void wv_buffer_damage_clear(struct wv_buffer* self)
{
	pixman_region_clear(&self->frame_damage);
}

struct wv_buffer_pool* wv_buffer_pool_create(
		const struct wv_buffer_config* config)
{
	struct wv_buffer_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	TAILQ_INIT(&self->queue);
	self->gbm_fd = -1;

	if (config)
		wv_buffer_pool_reconfig(self, config);

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
	free(pool->config.modifiers);
#ifdef ENABLE_SCREENCOPY_DMABUF
	if (pool->gbm)
		gbm_device_destroy(pool->gbm);
	if (pool->gbm_fd > 0)
		close(pool->gbm_fd);
#endif
	free(pool);
}

#ifdef ENABLE_SCREENCOPY_DMABUF
static int render_node_from_dev_t(char* node, size_t maxlen, dev_t device)
{
	drmDevice *dev_ptr;

	if (drmGetDeviceFromDevId(device, 0, &dev_ptr) < 0)
		return -1;

	if (dev_ptr->available_nodes & (1 << DRM_NODE_RENDER))
		strlcpy(node, dev_ptr->nodes[DRM_NODE_RENDER], maxlen);

	drmFreeDevice(&dev_ptr);

	return 0;
}

static int find_render_node(char *node, size_t maxlen) {
	bool r = -1;
	drmDevice *devices[64];

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		strlcpy(node, dev->nodes[DRM_NODE_RENDER], maxlen);
		r = 0;
		break;
	}

	drmFreeDevices(devices, n);
	return r;
}

static void open_render_node(struct wv_buffer_pool* pool)
{
	char path[256];
	if (major(pool->config.node) != 0 || minor(pool->config.node) != 0) {
		if (render_node_from_dev_t(path, sizeof(path),
					pool->config.node) < 0) {
			nvnc_log(NVNC_LOG_ERROR, "Could not find render node from dev_t");
			return;
		}
	} else if (find_render_node(path, sizeof(path)) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Could not find a render node");
		return;
	}

	nvnc_log(NVNC_LOG_DEBUG, "Using render node: %s", path);

	pool->gbm_fd = open(path, O_RDWR);
	if (pool->gbm_fd < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to open render node %s: %m",
				path);
		return;
	}

	pool->gbm = gbm_create_device(pool->gbm_fd);
	if (!pool->gbm) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to create a GBM device");
	}
}
#endif // ENABLE_SCREENCOPY_DMABUF

void wv_buffer_pool_reconfig(struct wv_buffer_pool* pool,
		const struct wv_buffer_config* config)
{
	if (buffer_configs_match(&pool->config, config))
		return;

	wv_buffer_pool_clear(pool);
	dev_t old_node = pool->config.node;
	copy_buffer_config(&pool->config, config);

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (old_node != config->node) {
		if (pool->gbm)
			gbm_device_destroy(pool->gbm);
		pool->gbm = NULL;
		if (pool->gbm_fd > 0)
			close(pool->gbm_fd);
		pool->gbm_fd = -1;

		open_render_node(pool);
	}

	if (major(pool->config.node) == 0 && minor(pool->config.node) == 0 &&
			!pool->gbm)
		open_render_node(pool);

	assert(pool->gbm);
#endif
}

static bool wv_buffer_pool_match_buffer(struct wv_buffer_pool* pool,
		struct wv_buffer* buffer)
{
	if (pool->config.type != buffer->type)
		return false;

	switch (pool->config.type) {
	case WV_BUFFER_SHM:
		if (pool->config.stride != buffer->stride) {
			return false;
		}

#ifdef ENABLE_SCREENCOPY_DMABUF
		/* fall-through */
	case WV_BUFFER_DMABUF:
#endif
		if (pool->config.width != buffer->width
		    || pool->config.height != buffer->height
		    || pool->config.format != buffer->format
		    || pool->config.node != buffer->node
		    || !modifiers_match(pool->config.modifiers,
			    pool->config.n_modifiers, buffer->modifiers,
			    buffer->n_modifiers))
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

	buffer = wv_buffer_create(&pool->config, pool->gbm);
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

void wv_buffer_registry_damage_all(struct pixman_region16* region,
		enum wv_buffer_domain domain)
{
	if (domain == WV_BUFFER_DOMAIN_UNSPEC)
		return;

	struct wv_buffer *buffer;
	LIST_FOREACH(buffer, &buffer_registry, registry_link)
		if (buffer->domain == domain)
			pixman_region_union(&buffer->buffer_damage,
					&buffer->buffer_damage, region);
}
