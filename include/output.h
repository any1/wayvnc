/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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

#include <wayland-client.h>
#include <stdint.h>
#include <stdbool.h>

struct zxdg_output_v1;
struct zwlr_output_power_v1;

enum output_power_state {
	OUTPUT_POWER_UNKNOWN = 0,
	OUTPUT_POWER_OFF,
	OUTPUT_POWER_ON,
};

const char* output_power_state_name(enum output_power_state state);

struct output {
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
	enum output_power_state power;

	bool is_dimension_changed;
	bool is_transform_changed;

	void (*on_dimension_change)(struct output*);
	void (*on_transform_change)(struct output*);
	void (*on_power_change)(struct output*);

	void* userdata;
};

struct output* output_new(struct wl_output* wl_output, uint32_t id);
void output_destroy(struct output* output);
void output_set_xdg_output(struct output* output,
                           struct zxdg_output_v1* xdg_output);
void output_set_wlr_output_power(struct output* output,
                           struct zwlr_output_power_v1* wlr_output_power);
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

uint32_t output_get_transformed_width(const struct output* self);
uint32_t output_get_transformed_height(const struct output* self);

void output_transform_coord(const struct output* self,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t* dst_x, uint32_t* dst_y);
void output_transform_box_coord(const struct output* self,
                                uint32_t src_x0, uint32_t src_y0,
                                uint32_t src_x1, uint32_t src_y1,
                                uint32_t* dst_x0, uint32_t* dst_y0,
                                uint32_t* dst_x1, uint32_t* dst_y1);
