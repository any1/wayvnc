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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include "seat.h"
#include "strlcpy.h"

static void seat_capabilities(void* data, struct wl_seat* wl_seat,
                              uint32_t capabilities)
{
	struct seat* self = data;

	self->capabilities = capabilities;
}

static void seat_name(void* data, struct wl_seat* wl_seat, const char* name)
{
	struct seat* self = data;

	strlcpy(self->name, name, sizeof(self->name));
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

struct seat* seat_new(struct wl_seat* wl_seat, uint32_t id)
{
	struct seat* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->wl_seat = wl_seat;
	self->id = id;

	wl_seat_add_listener(wl_seat, &seat_listener, self);

	return self;
}

void seat_destroy(struct seat* self)
{
	wl_seat_destroy(self->wl_seat);
	free(self);
}

void seat_list_destroy(struct wl_list* list)
{
	struct seat* seat;
	struct seat* tmp;

	wl_list_for_each_safe(seat, tmp, list, link) {
		wl_list_remove(&seat->link);
		seat_destroy(seat);
	}
}

struct seat* seat_find_by_name(struct wl_list* list, const char* name)
{
	struct seat* seat;

	wl_list_for_each(seat, list, link)
		if (strcmp(seat->name, name) == 0)
			return seat;

	return NULL;
}

struct seat* seat_find_by_id(struct wl_list* list, uint32_t id)
{
	struct seat* seat;

	wl_list_for_each(seat, list, link)
		if (seat->id == id)
			return seat;

	return NULL;
}

struct seat* seat_first(struct wl_list* list)
{
	struct seat* seat;

	wl_list_for_each(seat, list, link)
		return seat;

	return NULL;
}
