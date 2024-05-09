/*
 * Copyright (c) 2024 Andri Yngvason
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

#include <wayland-client-protocol.h>

struct virtual_keyboard_impl {
	struct virtual_keyboard* (*create)(struct wl_seat*);
	void (*destroy)(struct virtual_keyboard*);
	void (*keymap)(struct virtual_keyboard*, int fd, size_t size);
	void (*modifiers)(struct virtual_keyboard*, uint32_t depressed,
			uint32_t latched, uint32_t locked, uint32_t group);
	void (*key)(struct virtual_keyboard*, uint32_t time, uint32_t key,
			enum wl_keyboard_key_state);
};

struct virtual_keyboard {
	struct virtual_keyboard_impl* impl;
};
