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

#include "virtual-keyboard-unstable-v1.h"

#include <stdlib.h>
#include <assert.h>

extern struct zwp_virtual_keyboard_manager_v1* purism_virtual_keyboard_manager;
struct virtual_keyboard_impl purism_virtual_keyboard_impl;

struct purism_virtual_keyboard {
	struct virtual_keyboard base;
	struct zwp_virtual_keyboard_v1 *kb;
};

static struct virtual_keyboard* purism_virtual_keyboard_create(struct wl_seat* seat)
{
	struct purism_virtual_keyboard *self = calloc(1, sizeof(*self));
	assert(self);
	self->base.impl = &purism_virtual_keyboard_impl;
	self->kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			purism_virtual_keyboard_manager, seat);
	return &self->base;
}

static void purism_virtual_keyboard_destroy(struct virtual_keyboard* base)
{
	struct purism_virtual_keyboard* self =
		(struct purism_virtual_keyboard*)base;
	zwp_virtual_keyboard_v1_destroy(self->kb);
	free(self);
}

static void purism_virtual_keyboard_keymap(struct virtual_keyboard* base,
		int fd, size_t size)
{
	struct purism_virtual_keyboard* self =
		(struct purism_virtual_keyboard*)base;
	zwp_virtual_keyboard_v1_keymap(self->kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
			fd, size);
}

static void purism_virtual_keyboard_modifiers(struct virtual_keyboard* base,
		uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group)
{
	struct purism_virtual_keyboard* self =
		(struct purism_virtual_keyboard*)base;
	zwp_virtual_keyboard_v1_modifiers(self->kb, depressed, latched, locked,
			group);
}

static void purism_virtual_keyboard_key(struct virtual_keyboard* base,
		uint32_t time, uint32_t key, enum wl_keyboard_key_state state)
{
	struct purism_virtual_keyboard* self =
		(struct purism_virtual_keyboard*)base;
	zwp_virtual_keyboard_v1_key(self->kb, time, key, state);
}

struct virtual_keyboard_impl purism_virtual_keyboard_impl = {
	.create = purism_virtual_keyboard_create,
	.destroy = purism_virtual_keyboard_destroy,
	.keymap = purism_virtual_keyboard_keymap,
	.modifiers = purism_virtual_keyboard_modifiers,
	.key = purism_virtual_keyboard_key,
};
