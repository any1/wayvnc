/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
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

#pragma once

#include "image-source.h"

#include <wayland-client.h>
#include <stdint.h>
#include <stdbool.h>

struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zwlr_output_power_manager_v1;
struct zwlr_output_power_v1;

struct output {
	struct image_source image_source;

	struct wl_output* wl_output;
	struct zxdg_output_v1* xdg_output;
	struct zwlr_output_power_v1* wlr_output_power;
	struct wl_list link;

	uint32_t id;

	uint32_t width;
	uint32_t height;

	uint32_t x;
	uint32_t y;

	enum wl_output_transform transform;

	char make[256];
	char model[256];
	char name[256];
	char description[256];
	enum image_source_power_state power;

	bool is_headless;

	void* userdata;
};

struct output* output_from_image_source(const struct image_source* source);

struct output* output_new(struct wl_output* wl_output, uint32_t id);
void output_destroy(struct output* output);
void output_setup_xdg_output_managers(struct wl_list* list);
int output_acquire_power_on(struct output* output);
void output_release_power_on(struct output* output);
void output_list_destroy(struct wl_list* list);
struct output* output_find_by_id(struct wl_list* list, uint32_t id);
struct output* output_find_by_name(struct wl_list* list, const char* name);
struct output* output_first(struct wl_list* list);

enum output_cycle_direction {
	OUTPUT_CYCLE_FORWARD,
	OUTPUT_CYCLE_REVERSE,
};
struct output* output_cycle(const struct wl_list* list,
		const struct output* current,
		enum output_cycle_direction);
