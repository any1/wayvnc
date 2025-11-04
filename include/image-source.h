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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <wayland-client-protocol.h>

struct output;
struct wl_list;
struct image_source_impl;

enum image_source_power_state {
	IMAGE_SOURCE_POWER_UNKNOWN = 0,
	IMAGE_SOURCE_POWER_OFF,
	IMAGE_SOURCE_POWER_ON,
};

struct image_source {
	struct image_source_impl* impl;

	void (*on_transform_change)(struct image_source*);
	void (*on_power_change)(struct image_source*);
	void *userdata;
};

struct image_source_impl {
	void (*get_dimensions)(const struct image_source* self,
			int* width, int* height);
	enum wl_output_transform (*get_transform)(const struct image_source* self);
	enum image_source_power_state (*get_power_state)(
			const struct image_source* self);
	void (*describe)(const struct image_source*, char* description,
			size_t maxlen);
	int (*acquire_power_on)(struct image_source*);
	void (*release_power_on)(struct image_source*);
	void (*output_added)(struct image_source*, struct output*);
	void (*deinit)(struct image_source*);
};

void image_source_init(struct image_source* self, struct image_source_impl* impl);
void image_source_deinit(struct image_source* self);

void image_source_destroy(struct image_source* self);

void image_source_notify_output_added(struct image_source* self, struct output*);

bool image_source_is_output(const struct image_source* self);
bool image_source_is_toplevel(const struct image_source* self);
bool image_source_is_desktop(const struct image_source* self);

bool image_source_get_dimensions(const struct image_source* self,
		int* width, int* height);
bool image_source_get_transformed_dimensions(const struct image_source* self,
		int* width, int* height);
enum wl_output_transform image_source_get_transform(
		const struct image_source* self);
enum image_source_power_state image_source_get_power(
		const struct image_source* self);
const char* image_source_describe(const struct image_source* self,
		char* description, size_t maxlen);

int image_source_acquire_power_on(struct image_source* self);
void image_source_release_power_on(struct image_source* self);

const char* image_source_power_state_name(enum image_source_power_state state);

void image_source_transform_coord(const struct image_source* self,
		uint32_t src_x, uint32_t src_y,
		uint32_t* dst_x, uint32_t* dst_y);
