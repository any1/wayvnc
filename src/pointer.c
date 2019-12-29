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

#include <stdint.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <linux/input-event-codes.h>

#include "pointer.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include "time-util.h"

int pointer_init(struct pointer* self)
{
	self->pointer =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(self->manager, NULL);

	zwlr_virtual_pointer_v1_axis_source(self->pointer,
					    WL_POINTER_AXIS_SOURCE_WHEEL);
	return 0;
}

void pointer_destroy(struct pointer* self)
{
	zwlr_virtual_pointer_v1_destroy(self->pointer);
}

static void pointer_set_button_mask(struct pointer* self, uint32_t t,
				    enum nvnc_button_mask mask)
{
	enum nvnc_button_mask diff = self->current_mask ^ mask;

	if (diff & NVNC_BUTTON_LEFT)
		zwlr_virtual_pointer_v1_button(self->pointer, t, BTN_LEFT,
					       !!(mask & NVNC_BUTTON_LEFT));
	if (diff & NVNC_BUTTON_MIDDLE)
		zwlr_virtual_pointer_v1_button(self->pointer, t, BTN_MIDDLE,
					       !!(mask & NVNC_BUTTON_MIDDLE));
	if (diff & NVNC_BUTTON_RIGHT)
		zwlr_virtual_pointer_v1_button(self->pointer, t, BTN_RIGHT,
					       !!(mask & NVNC_BUTTON_RIGHT));

	if ((diff & NVNC_SCROLL_UP) && !(mask & NVNC_SCROLL_UP))
		zwlr_virtual_pointer_v1_axis(self->pointer, t,
					     WL_POINTER_AXIS_VERTICAL_SCROLL,
					     1);

	if ((diff & NVNC_SCROLL_DOWN) && !(mask & NVNC_SCROLL_DOWN))
		zwlr_virtual_pointer_v1_axis(self->pointer, t,
					     WL_POINTER_AXIS_VERTICAL_SCROLL,
					     1);
	
	self->current_mask = mask;
}

void pointer_set(struct pointer* self, uint32_t x, uint32_t y,
		 enum nvnc_button_mask button_mask)
{
	uint32_t t = gettime_ms();

	if (x != self->current_x || y != self->current_y)
		zwlr_virtual_pointer_v1_motion_absolute(self->pointer, t, x, y,
							self->width,
							self->height);

	self->current_x = x;
	self->current_y = y;

	if (button_mask != self->current_mask)
		pointer_set_button_mask(self, t, button_mask);
}
