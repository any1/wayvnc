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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>
#include <neatvnc.h>
#include <uv.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <pixman.h>

#include "wlr-export-dmabuf-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "render.h"
#include "dmabuf.h"
#include "screencopy.h"
#include "strlcpy.h"
#include "logging.h"

struct output {
	struct wl_output* wl_output;
	struct wl_list link;

	uint32_t id;
	uint32_t width;
	uint32_t height;
	char make[256];
	char model[256];
};

struct wayvnc {
	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_list outputs;

	struct renderer renderer;
	const struct output* selected_output;

	struct dmabuf_capture dmabuf_backend;
	struct screencopy screencopy_backend;
	struct frame_capture* capture_backend;

	uv_poll_t wayland_poller;
	uv_prepare_t flusher;
	uv_signal_t signal_handler;
	uv_timer_t performance_timer;

	struct nvnc* nvnc;

	struct nvnc_fb* current_fb;
	struct nvnc_fb* last_fb;

	uint32_t n_frames;
};

void on_capture_done(struct dmabuf_capture* capture);
void on_screencopy_done(struct frame_capture* capture);

static void output_handle_geometry(void* data, struct wl_output* wl_output,
				   int32_t x, int32_t y, int32_t phys_width,
				   int32_t phys_height, int32_t subpixel,
				   const char* make, const char* model,
				   int32_t transform)
{
	struct output* output = data;

	strlcpy(output->make, make, sizeof(output->make));
	strlcpy(output->model, model, sizeof(output->make));
}

static void output_handle_mode(void* data, struct wl_output* wl_output,
			       uint32_t flags, int32_t width, int32_t height,
			       int32_t refresh)
{
	struct output* output = data;

	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;

	output->width = width;
	output->height = height;
}

static void output_handle_done(void* data, struct wl_output* wl_output)
{
}

static void output_handle_scale(void* data, struct wl_output* wl_output,
				int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
};

void output_destroy(struct output* output)
{
	wl_output_destroy(output->wl_output);
	free(output);
}

struct output* wayvnc_output_find(struct wayvnc* self, uint32_t id)
{
	struct output* output;

	wl_list_for_each(output, &self->outputs, link)
		if (output->id == id)
			return output;

	return NULL;
}

void output_list_destroy(struct wl_list* list)
{
	struct output* output;
	struct output* tmp;

	wl_list_for_each_safe(output, tmp, list, link) {
		wl_list_remove(&output->link);
		output_destroy(output);
	}
}

static void registry_add(void* data, struct wl_registry* registry,
			 uint32_t id, const char* interface,
			 uint32_t version)
{
	struct wayvnc* self = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output* output = calloc(1, sizeof(*output));
		if (!output) {
			log_error("OOM\n");
			return;
		}

		output->id = id;
		output->wl_output = wl_registry_bind(registry, id,
						     &wl_output_interface,
						     version);

		wl_output_add_listener(output->wl_output, &output_listener,
				       output);
		wl_list_insert(&self->outputs, &output->link);
		return;
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		self->screencopy_backend.wl_shm
			= wl_registry_bind(registry, id, &wl_shm_interface, 1);
		return;
	}

	if (strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
		self->dmabuf_backend.manager =
			wl_registry_bind(registry, id,
					 &zwlr_export_dmabuf_manager_v1_interface,
					 1);
		return;
	}

	if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		self->screencopy_backend.manager =
			wl_registry_bind(registry, id,
					 &zwlr_screencopy_manager_v1_interface,
					 2);
		return;
	}
}

static void registry_remove(void* data, struct wl_registry* registry,
			    uint32_t id)
{
	struct wayvnc* self = data;

	struct output* out = wayvnc_output_find(self, id);
	if (out) {
		wl_list_remove(&out->link);
		output_destroy(out);

		/* TODO: If this is the selected output, exit */
	}
}

void wayvnc_destroy(struct wayvnc* self)
{
	output_list_destroy(&self->outputs);
	zwlr_export_dmabuf_manager_v1_destroy(self->dmabuf_backend.manager);
	wl_display_disconnect(self->display);
}

static int init_wayland(struct wayvnc* self)
{
	static const struct wl_registry_listener registry_listener = {
		.global = registry_add,
		.global_remove = registry_remove,
	};

	self->display = wl_display_connect(NULL);
	if (!self->display)
		return -1;

	wl_list_init(&self->outputs);

	self->registry = wl_display_get_registry(self->display);
	if (!self->registry)
		goto failure;

	wl_registry_add_listener(self->registry, &registry_listener, self);

	wl_display_roundtrip(self->display);
	wl_display_dispatch(self->display);

	if (!self->dmabuf_backend.manager) {
		log_error("Compositor does not support %s.\n",
			   zwlr_export_dmabuf_manager_v1_interface.name);
		goto export_manager_failure;
	}

	self->dmabuf_backend.on_done = on_capture_done;
	self->dmabuf_backend.userdata = self;

	self->screencopy_backend.frame_capture.on_done = on_screencopy_done;
	self->screencopy_backend.frame_capture.userdata = self;

	return 0;

export_manager_failure:
	output_list_destroy(&self->outputs);
failure:
	wl_display_disconnect(self->display);
	return -1;
}

int wayvnc_select_first_output(struct wayvnc* self)
{
	struct output* out;

	wl_list_for_each(out, &self->outputs, link) {
		self->selected_output = out;
		self->dmabuf_backend.output = out->wl_output;
		self->screencopy_backend.frame_capture.wl_output
			= out->wl_output;
		return 0;
	}

	return -1;
}

void on_wayland_event(uv_poll_t* handle, int status, int event)
{
	struct wayvnc* self = wl_container_of(handle, self, wayland_poller);
	wl_display_dispatch(self->display);
}

void prepare_for_poll(uv_prepare_t* handle)
{
	struct wayvnc* self = wl_container_of(handle, self, flusher);
	wl_display_flush(self->display);
}

void on_uv_walk(uv_handle_t* handle, void* arg)
{
	uv_unref(handle);
	uv_close(handle, NULL);
}

void wayvnc_exit(void)
{
	uv_walk(uv_default_loop(), on_uv_walk, NULL);
}

void on_signal(uv_signal_t* handle, int signo)
{
	wayvnc_exit();
}

void on_performance_tick(uv_timer_t* handle)
{
	struct wayvnc* self = wl_container_of(handle, self, performance_timer);

	printf("%"PRIu32" FPS\n", self->n_frames);
	self->n_frames = 0;
}

int init_main_loop(struct wayvnc* self)
{
	uv_loop_t* loop = uv_default_loop();

	uv_poll_init(loop, &self->wayland_poller,
		     wl_display_get_fd(self->display));
	uv_poll_start(&self->wayland_poller, UV_READABLE, on_wayland_event);

	uv_prepare_init(loop, &self->flusher);
	uv_prepare_start(&self->flusher, prepare_for_poll);

	uv_signal_init(loop, &self->signal_handler);
	uv_signal_start(&self->signal_handler, on_signal, SIGINT);

	uv_timer_init(loop, &self->performance_timer);
	uv_timer_start(&self->performance_timer, on_performance_tick, 1000,
		       1000);

	return 0;
}

uint32_t fourcc_from_gl_format(uint32_t format)
{
	switch (format) {
	case GL_BGRA_EXT: return DRM_FORMAT_XRGB8888;
	case GL_RGBA: return DRM_FORMAT_XBGR8888;
	}

	return DRM_FORMAT_INVALID;
}

int init_nvnc(struct wayvnc* self, const char* addr, uint16_t port)
{
	self->nvnc = nvnc_open(addr, port);
	if (!self->nvnc)
		return -1;

	nvnc_set_userdata(self->nvnc, self);

	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);
	if (format == DRM_FORMAT_INVALID)
		return -1;

	nvnc_set_name(self->nvnc, "WayVNC");
	nvnc_set_dimensions(self->nvnc,
			    self->selected_output->width,
			    self->selected_output->height,
			    format);

	return 0;
}

int wayvnc_start_capture(struct wayvnc* self)
{
	return frame_capture_start(self->capture_backend);
//	return dmabuf_capture_start(&self->dmabuf_backend);
}

void on_damage_check_done(struct pixman_region16* damage, void* userdata)
{
	struct wayvnc* self = userdata;

	if (pixman_region_not_empty(damage))
		nvnc_feed_frame(self->nvnc, self->current_fb, damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit();
	}
}

void wayvnc_update_vnc(struct wayvnc* self, struct nvnc_fb* fb)
{
	uint32_t width = nvnc_fb_get_width(fb);
	uint32_t height = nvnc_fb_get_height(fb);

	if (self->last_fb)
		nvnc_fb_unref(self->last_fb);

	self->last_fb = self->current_fb;
	self->current_fb = fb;

	if (self->last_fb) {
		uint32_t hint_x = self->capture_backend->damage_hint.x;
		uint32_t hint_y = self->capture_backend->damage_hint.y;
		uint32_t hint_width = self->capture_backend->damage_hint.width;
		uint32_t hint_height = self->capture_backend->damage_hint.height;

		nvnc_check_damage(self->current_fb, self->last_fb, hint_x,
				  hint_y, hint_width, hint_height,
				  on_damage_check_done, self);
		return;
	}

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, width, height);
	nvnc_feed_frame(self->nvnc, self->current_fb, &damage);
	pixman_region_fini(&damage);

	if (wayvnc_start_capture(self) < 0) {
		log_error("Failed to start capture. Exiting...\n");
		wayvnc_exit();
	}
}

void wayvnc_process_frame(struct wayvnc* self, struct dmabuf_frame* frame)
{
	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);

	struct nvnc_fb* fb = nvnc_fb_new(frame->width, frame->height, format);
	void* addr = nvnc_fb_get_addr(fb);

	render_dmabuf_frame(&self->renderer, frame);
	render_copy_pixels(&self->renderer, addr, 0, frame->height);

	wayvnc_update_vnc(self, fb);
}

void wayvnc_process_screen(struct wayvnc* self)
{
	uint32_t format = fourcc_from_gl_format(self->renderer.read_format);

	void* pixels = self->screencopy_backend.pixels;

	uint32_t width = self->capture_backend->frame_info.width;
	uint32_t height = self->capture_backend->frame_info.height;
	uint32_t stride = self->capture_backend->frame_info.stride;

	struct nvnc_fb* fb = nvnc_fb_new(width, height, format);
	void* addr = nvnc_fb_get_addr(fb);

	render_framebuffer(&self->renderer, pixels, format, width, height,
			   stride);
	render_copy_pixels(&self->renderer, addr, 0, height);

	wayvnc_update_vnc(self, fb);
}

void on_capture_done(struct dmabuf_capture* capture)
{
	struct wayvnc* self = capture->userdata;

	switch (capture->status) {
	case DMABUF_CAPTURE_IN_PROGRESS:
		break;
	case DMABUF_CAPTURE_FATAL:
		log_error("Fatal error while capturing. Exiting...\n");
		wayvnc_exit();
		break;
	case DMABUF_CAPTURE_CANCELLED:
		if (wayvnc_start_capture(self) < 0) {
			log_error("Failed to start capture. Exiting...\n");
			wayvnc_exit();
		}
		break;
	case DMABUF_CAPTURE_DONE:
		self->n_frames++;
		wayvnc_process_frame(self, &capture->frame);
		break;
	case DMABUF_CAPTURE_UNSPEC:
		abort();
	}
}

void on_screencopy_done(struct frame_capture* capture)
{
	struct wayvnc* self = capture->userdata;

	switch (capture->status) {
	case CAPTURE_STOPPED:
		break;
	case CAPTURE_IN_PROGRESS:
		break;
	case CAPTURE_FATAL:
		log_error("Fatal error while capturing. Exiting...\n");
		wayvnc_exit();
		break;
	case CAPTURE_FAILED:
		if (wayvnc_start_capture(self) < 0) {
			log_error("Failed to start capture. Exiting...\n");
			wayvnc_exit();
		}
		break;
	case CAPTURE_DONE:
		self->n_frames++;
		wayvnc_process_screen(self);
		break;
	}
}

int main(int argc, char* argv[])
{
	struct wayvnc self = { 0 };

	if (init_wayland(&self) < 0) {
		log_error("Failed to initialise wayland\n");
		return 1;
	}

	printf("Outputs:\n");

	struct output* out;
	wl_list_for_each(out, &self.outputs, link)
		printf("%"PRIu32": Make: %s. Model: %s\n", out->id, out->make,
		       out->model);

	/* TODO: Allow selecting output */
	if (wayvnc_select_first_output(&self) < 0) {
		log_error("No output found\n");
		goto failure;
	}

	if (renderer_init(&self.renderer, self.selected_output->width,
			  self.selected_output->height) < 0) {
		log_error("Failed to initialise renderer\n");
		goto failure;
	}

	if (init_main_loop(&self) < 0)
		goto main_loop_failure;

	if (init_nvnc(&self, "0.0.0.0", 5900) < 0)
		goto nvnc_failure;

	screencopy_init(&self.screencopy_backend);
	self.capture_backend = &self.screencopy_backend.frame_capture;

	if (wayvnc_start_capture(&self) < 0)
		goto capture_failure;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

//	dmabuf_capture_stop(&self.dmabuf_backend);

	if (self.current_fb) nvnc_fb_unref(self.current_fb);
	if (self.last_fb) nvnc_fb_unref(self.last_fb);

	nvnc_close(self.nvnc);
	renderer_destroy(&self.renderer);
	wayvnc_destroy(&self);
	return 0;

capture_failure:
	nvnc_close(self.nvnc);
nvnc_failure:
main_loop_failure:
	renderer_destroy(&self.renderer);
failure:
	wayvnc_destroy(&self);
	return 1;
}
