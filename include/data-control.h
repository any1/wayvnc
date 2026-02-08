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
#include <stdbool.h>
#include <stddef.h>

#include "sys/queue.h"

struct wl_display;
struct wl_seat;
struct zwlr_data_control_manager_v1;
struct zwlr_data_control_device_v1;
struct zwlr_data_control_source_v1;
struct zwlr_data_control_offer_v1;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_source_v1;
struct ext_data_control_offer_v1;
struct receive_context;
struct send_context;

LIST_HEAD(receive_context_list, receive_context);
LIST_HEAD(send_context_list, send_context);

enum data_control_protocol {
	DATA_CONTROL_PROTOCOL_NONE = 0,
	DATA_CONTROL_PROTOCOL_WLR,
	DATA_CONTROL_PROTOCOL_EXT,
};

struct data_control {
	struct wl_display* wl_display;
	struct nvnc* server;
	struct receive_context_list receive_contexts;
	struct send_context_list send_contexts;
	enum data_control_protocol protocol;
	struct {
		struct zwlr_data_control_manager_v1* manager;
		struct zwlr_data_control_device_v1* device;
		struct zwlr_data_control_source_v1* selection;
		struct zwlr_data_control_source_v1* primary_selection;
		struct zwlr_data_control_offer_v1* offer;
	} wlr;
	struct {
		struct ext_data_control_manager_v1* manager;
		struct ext_data_control_device_v1* device;
		struct ext_data_control_source_v1* selection;
		struct ext_data_control_source_v1* primary_selection;
		struct ext_data_control_offer_v1* offer;
	} ext;
	bool is_own_offer;
	const char* mime_type;
	/* x-wayvnc-client-(8 hexadecimal digits) + \0 */
	char custom_mime_type_name[32];
	char* cb_data;
	size_t cb_len;
};

void data_control_init(struct data_control* self, enum data_control_protocol protocol,
		struct zwlr_data_control_manager_v1* wlr_manager,
		struct ext_data_control_manager_v1* ext_manager,
		struct nvnc* server, struct wl_seat* seat);
void data_control_destroy(struct data_control* self);
void data_control_to_clipboard(struct data_control* self, const char* text, size_t len);
