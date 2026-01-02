/*
 * Copyright (c) 2025 Andri Yngvason
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

#include "desktop.h"
#include "output.h"
#include "wayland.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <neatvnc.h>

static void desktop_capture_handle_done(enum screencopy_result result,
		struct wv_buffer* buffer, struct image_source* source,
		void* userdata);
static double desktop_capture_rate_format(const void* userdata,
		enum wv_buffer_type type, uint32_t format, uint64_t modifier);

extern struct wayland* wayland;

struct desktop* desktop_from_image_source(const struct image_source* source)
{
	assert(image_source_is_desktop(source));
	struct desktop* self = wl_container_of(source, self, image_source);
	return self;
}

void desktop_destroy(struct desktop *self)
{
	image_source_destroy(&self->image_source);
}

static void desktop_image_source_get_dimensions(const struct image_source* base,
		int* width_out, int* height_out)
{
	struct desktop* self = desktop_from_image_source(base);

	int width = 0;
	int height = 0;

	struct desktop_output* output;
	LIST_FOREACH(output, &self->outputs, link) {
		int w = output->output->x + output->output->width;
		int h = output->output->y + output->output->height;
		if (w > width)
			width = w;
		if (h > height)
			height = h;
	}

	if (width_out)
		*width_out = width;
	if (height_out)
		*height_out = height;
}

static enum image_source_power_state desktop_image_source_get_power_state(
		const struct image_source* base)
{
	struct desktop* self = desktop_from_image_source(base);
	enum image_source_power_state state = IMAGE_SOURCE_POWER_UNKNOWN;

	struct desktop_output* output = LIST_FIRST(&self->outputs);
	if (!output)
		return IMAGE_SOURCE_POWER_UNKNOWN;

	state = image_source_get_power(&output->output->image_source);

	for (output = LIST_NEXT(output, link); output;
			output = LIST_NEXT(output, link)) {
		enum image_source_power_state this_state =
			image_source_get_power(&output->output->image_source);
		if (this_state != state) {
			nvnc_log(NVNC_LOG_WARNING, "Power state mismatch between outputs");
			return IMAGE_SOURCE_POWER_UNKNOWN;
		}
	}

	nvnc_log(NVNC_LOG_DEBUG, "Returing power state: %s\n",
			image_source_power_state_name(state));
	return state;
}

static void desktop_image_source_describe(const struct image_source* base,
		char* dst, size_t maxlen)
{
	snprintf(dst, maxlen, "Desktop");
}

static int desktop_image_source_acquire_power_on(struct image_source* base)
{
	struct desktop* self = desktop_from_image_source(base);

	int status = 0;
	struct desktop_output* output;
	LIST_FOREACH(output, &self->outputs, link) {
		int rc = output_acquire_power_on(output->output);
		if (rc < 0)
			goto fail;

		if (rc > 0)
			status = 1;
	}

	return status;

fail:
	LIST_FOREACH(output, &self->outputs, link)
		output_release_power_on(output->output);
	return -1;
}

static void desktop_image_source_release_power_on(struct image_source* base)
{
	struct desktop* self = desktop_from_image_source(base);

	struct desktop_output* output;
	LIST_FOREACH(output, &self->outputs, link)
		output_release_power_on(output->output);
}

static void desktop_image_source_output_power_change(struct observer* observer,
		void *arg)
{
	struct desktop_output* event_source = wl_container_of(observer,
			event_source, power_change_observer);
	struct desktop* desktop = event_source->desktop;

	int n = 0, n_off = 0, n_on = 0;
	struct desktop_output* output;
	LIST_FOREACH(output, &desktop->outputs, link) {
		n++;

		enum image_source_power_state state;
		state = image_source_get_power(&output->output->image_source);
		switch (state) {
		case IMAGE_SOURCE_POWER_ON:
			n_on++;
			break;
		case IMAGE_SOURCE_POWER_OFF:
			n_off++;
			break;
		default:
			break;
		}
	}

	if (n_on == n || n_off == n) {
		nvnc_log(NVNC_LOG_DEBUG, "Desktop power state changed\n");
		observable_notify(&desktop->image_source.observable.power_change,
				NULL);
	}
}

static void desktop_image_source_output_geometry_change(struct observer* observer,
		void *arg)
{
	struct desktop_output* event_source = wl_container_of(observer,
			event_source, geometry_change_observer);
	struct desktop* desktop = event_source->desktop;
	observable_notify(&desktop->image_source.observable.geometry_change,
			NULL);
}

static struct desktop_output *desktop_output_create(struct output* output,
		struct desktop* desktop)
{
	struct desktop_output* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->output = output;
	self->desktop = desktop;

	observer_init(&self->power_change_observer,
			&output->image_source.observable.power_change,
			desktop_image_source_output_power_change);

	observer_init(&self->geometry_change_observer,
			&output->image_source.observable.geometry_change,
			desktop_image_source_output_geometry_change);

	struct desktop_capture* capture = desktop->capture;
	if (capture) {
		struct screencopy *sc = screencopy_create(&output->image_source,
				capture->render_cursor);

		sc->userdata = capture;
		sc->on_done = desktop_capture_handle_done;
		sc->rate_format = desktop_capture_rate_format;
		self->sc = sc;
	}

	return self;
}

static void desktop_output_destroy(struct desktop_output* self)
{
	screencopy_destroy(self->sc);
	observer_deinit(&self->power_change_observer);
	observer_deinit(&self->geometry_change_observer);
	free(self);
}

static void desktop_image_source_output_added(struct observer* observer,
		void* arg)
{
	struct output* output = arg;
	struct desktop* self = wl_container_of(observer, self,
			output_added_observer);

	struct desktop_output *desktop_output =
		desktop_output_create(output, self);
	assert(desktop_output);

	LIST_INSERT_HEAD(&self->outputs, desktop_output, link);

	observable_notify(&self->image_source.observable.geometry_change, NULL);
}

static void desktop_image_source_output_removed(struct observer* observer,
		void* arg)
{
	struct output* output = arg;
	struct desktop* self = wl_container_of(observer, self,
			output_removed_observer);

	struct desktop_output* desktop_output = NULL;
	LIST_FOREACH(desktop_output, &self->outputs, link)
		if (desktop_output->output == output)
			break;

	assert(desktop_output && desktop_output->output == output);

	LIST_REMOVE(desktop_output, link);
	desktop_output_destroy(desktop_output);

	observable_notify(&self->image_source.observable.geometry_change, NULL);
}

static void desktop_image_source_deinit(struct image_source* base)
{
	struct desktop* self = desktop_from_image_source(base);

	if (self->capture)
		self->capture->desktop = NULL;

	while (!LIST_EMPTY(&self->outputs)) {
		struct desktop_output* output = LIST_FIRST(&self->outputs);
		LIST_REMOVE(output, link);
		desktop_output_destroy(output);
	}

	observer_deinit(&self->output_added_observer);
	observer_deinit(&self->output_removed_observer);
}

static struct image_source_impl image_source_impl = {
	.get_dimensions = desktop_image_source_get_dimensions,
	.get_power_state = desktop_image_source_get_power_state,
	.describe = desktop_image_source_describe,
	.acquire_power_on = desktop_image_source_acquire_power_on,
	.release_power_on = desktop_image_source_release_power_on,
	.deinit = desktop_image_source_deinit,
};

bool image_source_is_desktop(const struct image_source* source)
{
	return source->impl == &image_source_impl;
}

struct desktop* desktop_new(struct wl_list* output_list)
{
	struct desktop* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	observer_init(&self->output_added_observer,
			&wayland->observable.output_added,
			desktop_image_source_output_added);

	observer_init(&self->output_removed_observer,
			&wayland->observable.output_removed,
			desktop_image_source_output_removed);

	LIST_INIT(&self->outputs);

	struct output* output;
	wl_list_for_each(output, output_list, link) {
		struct desktop_output *desktop_output =
			desktop_output_create(output, self);
		assert(desktop_output);

		LIST_INSERT_HEAD(&self->outputs, desktop_output, link);
	}

	image_source_init(&self->image_source, &image_source_impl);

	return self;
}

struct screencopy_impl desktop_capture_impl;

static void desktop_capture_handle_done(enum screencopy_result result,
		struct wv_buffer* buffer, struct image_source* source,
		void* userdata)
{
	struct desktop_capture* self = userdata;

	// TODO: Maybe extra handling is needed for failures here?

	self->base.on_done(result, buffer, source, self->base.userdata);
}

static double desktop_capture_rate_format(const void* userdata,
		enum wv_buffer_type type, uint32_t format, uint64_t modifier)
{
	const struct desktop_capture* self = userdata;
	if (self->base.rate_format)
		return self->base.rate_format(self->base.userdata, type, format,
				modifier);
	return 1;
}

static struct screencopy* desktop_capture_create(struct image_source* source,
		bool render_cursor)
{
	assert(image_source_is_desktop(source));

	struct desktop_capture* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.impl = &desktop_capture_impl;
	self->base.rate_limit = 30;
	self->render_cursor = render_cursor;

	struct desktop* desktop = desktop_from_image_source(source);
	self->desktop = desktop;

	assert(!desktop->capture);
	desktop->capture = self;

	struct desktop_output* desktop_output;
	LIST_FOREACH(desktop_output, &desktop->outputs, link) {
		struct output* output = desktop_output->output;
		struct screencopy *sc = screencopy_create(&output->image_source,
				render_cursor);

		sc->userdata = self;
		sc->on_done = desktop_capture_handle_done;
		sc->rate_format = desktop_capture_rate_format;
		desktop_output->sc = sc;
	}

	return (struct screencopy*)self;
}

static void desktop_capture_destroy(struct screencopy* base)
{
	struct desktop_capture* self = (struct desktop_capture*)base;

	struct desktop* desktop = self->desktop;
	if (desktop) {
		desktop->capture = NULL;

		struct desktop_output* desktop_output;
		LIST_FOREACH(desktop_output, &desktop->outputs, link) {
			screencopy_destroy(desktop_output->sc);
			desktop_output->sc = NULL;
		}
	}

	free(self);
}

static int desktop_capture_start(struct screencopy* base, bool immediate)
{
	struct desktop_capture* self = (struct desktop_capture*)base;
	struct desktop* desktop = self->desktop;
	if (!desktop)
		return -1;

	struct desktop_output* desktop_output;
	LIST_FOREACH(desktop_output, &desktop->outputs, link) {
		struct screencopy* sc = desktop_output->sc;
		sc->rate_limit = base->rate_limit;
		sc->enable_linux_dmabuf = base->enable_linux_dmabuf;

		int rc = screencopy_start(sc, immediate);
		if (rc != 0) {
			return -1;
		}
	}

	return 0;
}

static void desktop_capture_stop(struct screencopy* base)
{
	struct desktop_capture* self = (struct desktop_capture*)base;
	struct desktop* desktop = self->desktop;
	if (!desktop)
		return;

	struct desktop_output* desktop_output;
	LIST_FOREACH(desktop_output, &desktop->outputs, link) {
		struct screencopy* sc = desktop_output->sc;
		screencopy_stop(sc);
	}
}

static enum screencopy_capabilitites desktop_capture_get_caps(
		const struct screencopy* base)
{
	struct desktop_capture* self = (struct desktop_capture*)base;
	struct desktop* desktop = self->desktop;
	if (!desktop)
		return 0;

	if (LIST_EMPTY(&desktop->outputs)) {
		nvnc_log(NVNC_LOG_ERROR, "Whoops. No outputs. Can't get capabilities");
		return 0;
	}

	struct desktop_output* output = LIST_FIRST(&desktop->outputs);
	return screencopy_get_capabilities(output->sc);
}

struct screencopy_impl desktop_capture_impl = {
	.create = desktop_capture_create,
	.destroy = desktop_capture_destroy,
	.start = desktop_capture_start,
	.stop = desktop_capture_stop,
	.get_capabilities = desktop_capture_get_caps,
};
