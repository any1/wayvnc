#pragma once

struct output {
	struct wl_output* wl_output;
	struct wl_list link;

	uint32_t id;
	uint32_t width;
	uint32_t height;
	char make[256];
	char model[256];
};

struct output* output_new(struct wl_output* wl_output, uint32_t id);
void output_destroy(struct output* output);
void output_list_destroy(struct wl_list* list);
