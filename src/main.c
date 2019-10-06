#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>
#include <wayland-client.h>

#include "wlr-export-dmabuf-unstable-v1.h"
#include "render.h"
#include "dmabuf.h"
#include "strlcpy.h"
#include "logging.h"

struct wayvnc {
	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_list outputs;

	struct zwlr_export_dmabuf_manager_v1* export_manager;
};

struct output {
	struct wl_output* wl_output;
	struct wl_list link;

	uint32_t id;
	uint32_t width;
	uint32_t height;
	char make[256];
	char model[256];
};

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

	if (strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
		self->export_manager =
			wl_registry_bind(registry, id,
					 &zwlr_export_dmabuf_manager_v1_interface,
					 version);
		return;
	}
}

static void registry_remove(void* data, struct wl_registry* registry,
			    uint32_t id)
{
	/* TODO */
}

void wayvnc_destroy(struct wayvnc* self)
{
	output_list_destroy(&self->outputs);
	zwlr_export_dmabuf_manager_v1_destroy(self->export_manager);
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

	if (!self->export_manager) {
		log_error("Compositor does not support %s.\n",
			   zwlr_export_dmabuf_manager_v1_interface.name);
		goto export_manager_failure;
	}

	return 0;

export_manager_failure:
	output_list_destroy(&self->outputs);
failure:
	wl_display_disconnect(self->display);
	return -1;
}

int main(int argc, char* argv[])
{
	struct wayvnc self;

	if (init_wayland(&self) < 0)
		return 1;

	printf("Outputs:\n");

	struct output* out;
	wl_list_for_each(out, &self.outputs, link)
		printf("%"PRIu32": Make: %s. Model: %s\n", out->id, out->make,
		       out->model);

	return 0;
}
