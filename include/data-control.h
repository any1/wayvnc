/*
 * Copyright (c) 2020 Scott Moreau
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

#include <neatvnc.h>

#include "wlr-data-control-unstable-v1.h"

struct data_control {
	struct wl_display* wl_display;
	struct nvnc* server;
	struct zwlr_data_control_manager_v1* manager;
	struct zwlr_data_control_device_v1* device;
	struct zwlr_data_control_source_v1* selection;
	struct zwlr_data_control_source_v1* primary_selection;
	struct zwlr_data_control_offer_v1* offer;
	bool is_own_offer;
	const char* mime_type;
	/* x-wayvnc-client-(8 hexadecimal digits) + \0 */
	char custom_mime_type_name[32];
	char* cb_data;
	size_t cb_len;
};

void data_control_init(struct data_control* self, struct wl_display* wl_display, struct nvnc* server, struct wl_seat* seat);
void data_control_destroy(struct data_control* self);
void data_control_to_clipboard(struct data_control* self, const char* text, size_t len);
