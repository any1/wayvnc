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

#include <stddef.h>
#include "sys/queue.h"

struct observer;
struct observable;

typedef void (*observer_notify_fn)(struct observer*, void* arg);

struct observer {
	LIST_ENTRY(observer) link;
	struct observable* subject;
	observer_notify_fn notify;
};

LIST_HEAD(observable, observer);

static inline void observer_init(struct observer* self,
		struct observable* subject, observer_notify_fn notify)
{
	self->subject = subject;
	self->notify = notify;
	LIST_INSERT_HEAD(subject, self, link);
}

static inline void observer_deinit(struct observer* self)
{
	if (self->subject)
		LIST_REMOVE(self, link);
	self->subject = NULL;
}

static inline void observable_init(struct observable* self)
{
	LIST_INIT(self);
}

static inline void observable_deinit(struct observable* self)
{
	while (!LIST_EMPTY(self)) {
		struct observer* observer = LIST_FIRST(self);
		LIST_REMOVE(observer, link);
		observer->subject = NULL;
	}
}

static inline void observable_notify(struct observable* self, void* arg)
{
	struct observer* observer;
	struct observer* tmp;
	LIST_FOREACH_SAFE(observer, self, link, tmp)
		observer->notify(observer, arg);
}
