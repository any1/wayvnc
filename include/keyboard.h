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

#pragma once

#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>
#include <stdbool.h>
#include <neatvnc.h>

#include "intset.h"

struct zwp_virtual_keyboard_v1;
struct table_entry;
struct nvnc;

struct keyboard {
	struct zwp_virtual_keyboard_v1* virtual_keyboard;

	struct xkb_context* context;
	struct xkb_keymap* keymap;
	struct xkb_state* state;

	size_t lookup_table_size;
	size_t lookup_table_length;
	struct table_entry* lookup_table;

	struct intset key_state;

	int last_sent_group;
};

int keyboard_init(struct keyboard* self, const struct xkb_rule_names* rule_names);
void keyboard_destroy(struct keyboard* self);
void keyboard_feed(struct keyboard* self, xkb_keysym_t symbol, bool is_pressed);
void keyboard_feed_code(struct keyboard* self, xkb_keycode_t code,
		bool is_pressed);
enum nvnc_keyboard_led_state keyboard_get_led_state(const struct keyboard*);
