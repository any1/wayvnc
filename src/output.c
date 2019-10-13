#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <wayland-client.h>

#include "output.h"
#include "strlcpy.h"
#include "logging.h"

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

struct output* output_new(struct wl_output* wl_output, uint32_t id)
{
	struct output* output = calloc(1, sizeof(*output));
	if (!output) {
		log_error("OOM\n");
		return NULL;
	}

	output->wl_output = wl_output;
	output->id = id;

	wl_output_add_listener(output->wl_output, &output_listener,
			output);

	return output;
}
