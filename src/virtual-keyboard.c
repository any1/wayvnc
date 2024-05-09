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

#include "virtual-keyboard.h"
#include "virtual-keyboard-impl.h"

#include <assert.h>

extern struct virtual_keyboard_impl purism_virtual_keyboard_impl;
extern struct virtual_keyboard_impl ext_virtual_keyboard_impl;

struct virtual_keyboard *virtual_keyboard_create(struct wl_seat* seat)
{
	struct virtual_keyboard* kb = ext_virtual_keyboard_impl.create(seat);
	return kb ? kb : purism_virtual_keyboard_impl.create(seat);
}

void virtual_keyboard_destroy(struct virtual_keyboard* self)
{
	if (!self)
		return;
	assert(self->impl && self->impl->destroy);
	self->impl->destroy(self);
}

void virtual_keyboard_keymap(struct virtual_keyboard* self, int fd,
		size_t size)
{
	assert(self->impl && self->impl->keymap);
	self->impl->keymap(self, fd, size);
}

void virtual_keyboard_modifiers(struct virtual_keyboard* self,
		uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group)
{
	assert(self->impl && self->impl->modifiers);
	self->impl->modifiers(self, depressed, latched, locked, group);
}

void virtual_keyboard_key(struct virtual_keyboard* self, uint32_t time,
		uint32_t key, enum wl_keyboard_key_state state)
{
	assert(self->impl && self->impl->key);
	self->impl->key(self, time, key, state);
}

bool virtual_keyboard_repeat_info(struct virtual_keyboard* self, int32_t rate,
		int32_t delay)
{
	if (!self->impl->repeat_info)
		return false;
	self->impl->repeat_info(self, rate, delay);
	return true;
}
