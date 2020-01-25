/*
 * Copyright (c) 2020 Andri Yngvason
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

#include "intset.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_CAPACITY 256

int intset_init(struct intset* self, size_t cap)
{
	if (cap == 0)
		cap = DEFAULT_CAPACITY;

	memset(self, 0, sizeof(*self));

	self->storage = malloc(cap * sizeof(*self->storage));
	if (!self->storage)
		return -1;

	self->cap = cap;

	return 0;
}

void intset_destroy(struct intset* self)
{
	free(self->storage);
	memset(self, 0, sizeof(*self));
}

static int intset__grow(struct intset*  self)
{
	size_t new_cap = self->cap * 2;

	int32_t* new_storage = realloc(self->storage, new_cap);
	if (!new_storage)
		return -1;

	self->storage = new_storage;
	self->cap = new_cap;

	return 0;
}

int intset_set(struct intset* self, int32_t value)
{
	if (intset_is_set(self, value))
		return 0;

	if (self->len >= self->cap && intset__grow(self) < 0)
		return -1;

	self->storage[self->len++] = value;

	return 0;
}

static ssize_t intset__find_index(const struct intset* self, int32_t value)
{
	for (size_t i = 0; i < self->len; ++i)
		if (self->storage[i] == value)
			return i;

	return -1;
}

void intset_clear(struct intset* self, int32_t value)
{
	ssize_t index = intset__find_index(self, value);
	if (index < 0)
		return;

	self->storage[index] = self->storage[--self->len];
}

bool intset_is_set(const struct intset* self, int32_t value)
{
	return intset__find_index(self, value) >= 0;
}
