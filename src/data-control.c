/*
 * Copyright (c) 2020 Scott Moreau
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <aml.h>
#include <neatvnc.h>

#include "data-control.h"
#include "ext-data-control-v1.h"
#include "wlr-data-control-unstable-v1.h"

static const char custom_mime_type_data[] = "wayvnc";

struct receive_context {
	struct nvnc* server;
	struct aml_handler* handler;
	LIST_ENTRY(receive_context) link;
	int fd;
	FILE* mem_fp;
	size_t mem_size;
	char* mem_data;
};

struct send_context {
	struct aml_handler* handler;
	LIST_ENTRY(send_context) link;
	int fd;
	char* data;
	size_t length;
	size_t index;
};

static bool data_control_is_wlr(const struct data_control* self)
{
	return self->protocol == DATA_CONTROL_PROTOCOL_WLR;
}

static const struct zwlr_data_control_offer_v1_listener wlr_data_control_offer_listener;
static const struct ext_data_control_offer_v1_listener ext_data_control_offer_listener;
static const struct zwlr_data_control_device_v1_listener wlr_data_control_device_listener;
static const struct ext_data_control_device_v1_listener ext_data_control_device_listener;
static const struct zwlr_data_control_source_v1_listener wlr_data_control_source_listener;
static const struct ext_data_control_source_v1_listener ext_data_control_source_listener;

static void destroy_receive_context(struct receive_context* ctx)
{
	aml_stop(aml_get_default(), ctx->handler);
	aml_unref(ctx->handler);

	if (ctx->mem_fp)
		fclose(ctx->mem_fp);
	free(ctx->mem_data);
	close(ctx->fd);
	LIST_REMOVE(ctx, link);
	free(ctx);
}

static void destroy_send_context(struct send_context* ctx)
{
	aml_stop(aml_get_default(), ctx->handler);
	aml_unref(ctx->handler);

	close(ctx->fd);
	free(ctx->data);
	LIST_REMOVE(ctx, link);
	free(ctx);
}

static void on_receive(struct aml_handler* handler)
{
	struct receive_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	char buf[4096];

	ssize_t ret = read(fd, &buf, sizeof(buf));
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		nvnc_log(NVNC_LOG_ERROR, "Clipboard read failed: %m");
		destroy_receive_context(ctx);
	} else if (ret > 0) {
		fwrite(&buf, 1, ret, ctx->mem_fp);
		return;
	}

	fclose(ctx->mem_fp);
	ctx->mem_fp = NULL;

	if (ctx->mem_size)
		nvnc_send_cut_text(ctx->server, ctx->mem_data, ctx->mem_size);

	destroy_receive_context(ctx);
}

static void on_send(struct aml_handler* handler)
{
	struct send_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	int ret;
	ret = write(fd, ctx->data + ctx->index, ctx->length - ctx->index);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		nvnc_log(NVNC_LOG_ERROR, "Clipboard write failed/incomplete: %m");
		destroy_send_context(ctx);
	} else if (ret == (int)(ctx->length - ctx->index)) {
		destroy_send_context(ctx);
	} else {
		ctx->index += ret;
	}
}

static int dont_block(int fd)
{
	int ret = fcntl(fd, F_GETFL);
	if (ret == -1)
		return -1;
	return fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

static void receive_data(struct data_control* self)
{
	int pipe_fd[2];

	if (data_control_is_wlr(self)) {
		if (!self->wlr.offer)
			return;
	} else if (!self->ext.offer) {
		return;
	}

	if (pipe(pipe_fd) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "pipe() failed: %m");
		return;
	}

	if (dont_block(pipe_fd[0]) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to set O_NONBLOCK on clipbooard receive fd");
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	struct receive_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	if (data_control_is_wlr(self)) {
		zwlr_data_control_offer_v1_receive(
			self->wlr.offer, self->mime_type, pipe_fd[1]);
	} else {
		ext_data_control_offer_v1_receive(
			self->ext.offer, self->mime_type, pipe_fd[1]);
	}
	close(pipe_fd[1]);

	ctx->fd = pipe_fd[0];
	ctx->server = self->server;
	ctx->mem_fp = open_memstream(&ctx->mem_data, &ctx->mem_size);
	if (!ctx->mem_fp) {
		nvnc_log(NVNC_LOG_ERROR, "open_memstream() failed: %m");
		goto open_memstream_failure;
	}

	ctx->handler = aml_handler_new(ctx->fd, on_receive, ctx, NULL);
	if (!ctx->handler) {
		goto handler_failure;
	}

	if (aml_start(aml_get_default(), ctx->handler) < 0) {
		goto poll_start_failure;
	}

	LIST_INSERT_HEAD(&self->receive_contexts, ctx, link);
	return;

poll_start_failure:
	aml_unref(ctx->handler);
handler_failure:
	fclose(ctx->mem_fp);
open_memstream_failure:
	free(ctx);
	close(pipe_fd[0]);
}

static void data_control_offer_handle(struct data_control* self, const char* mime_type,
		struct zwlr_data_control_offer_v1* wlr_offer,
		struct ext_data_control_offer_v1* ext_offer)
{
	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		self->is_own_offer = true;
		return;
	}

	if (data_control_is_wlr(self)) {
		if (self->wlr.offer)
			return;

		if (strcmp(mime_type, self->mime_type) == 0)
			self->wlr.offer = wlr_offer;
	} else {
		if (self->ext.offer)
			return;

		if (strcmp(mime_type, self->mime_type) == 0)
			self->ext.offer = ext_offer;
	}
}

static void data_control_offer_wlr(void* data,
	struct zwlr_data_control_offer_v1* offer,
	const char* mime_type)
{
	data_control_offer_handle(data, mime_type, offer, NULL);
}

static void data_control_offer_ext(void* data,
	struct ext_data_control_offer_v1* offer,
	const char* mime_type)
{
	data_control_offer_handle(data, mime_type, NULL, offer);
}

static const struct zwlr_data_control_offer_v1_listener wlr_data_control_offer_listener = {
	.offer = data_control_offer_wlr
};

static const struct ext_data_control_offer_v1_listener ext_data_control_offer_listener = {
	.offer = data_control_offer_ext
};

static void data_control_device_offer_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	if (!offer)
		return;

	zwlr_data_control_offer_v1_add_listener(
		offer, &wlr_data_control_offer_listener, data);
}

static void data_control_device_offer_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	if (!offer)
		return;

	ext_data_control_offer_v1_add_listener(
		offer, &ext_data_control_offer_listener, data);
}

static void data_control_device_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	struct data_control* self = data;

	if (!offer) {
		if (self->wlr.offer) {
			zwlr_data_control_offer_v1_destroy(self->wlr.offer);
			self->wlr.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->wlr.offer && !self->is_own_offer)
		receive_data(self);

	zwlr_data_control_offer_v1_destroy(offer);
	self->wlr.offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_selection_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	struct data_control* self = data;

	if (!offer) {
		if (self->ext.offer) {
			ext_data_control_offer_v1_destroy(self->ext.offer);
			self->ext.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->ext.offer && !self->is_own_offer)
		receive_data(self);

	ext_data_control_offer_v1_destroy(offer);
	self->ext.offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_finished_wlr(void* data,
	struct zwlr_data_control_device_v1* device)
{
	zwlr_data_control_device_v1_destroy(device);
}

static void data_control_device_finished_ext(void* data,
	struct ext_data_control_device_v1* device)
{
	ext_data_control_device_v1_destroy(device);
}

static void data_control_device_primary_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	struct data_control* self = data;

	if (!offer) {
		if (self->wlr.offer) {
			zwlr_data_control_offer_v1_destroy(self->wlr.offer);
			self->wlr.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->wlr.offer && !self->is_own_offer)
		receive_data(self);

	zwlr_data_control_offer_v1_destroy(offer);
	self->wlr.offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_primary_selection_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	struct data_control* self = data;

	if (!offer) {
		if (self->ext.offer) {
			ext_data_control_offer_v1_destroy(self->ext.offer);
			self->ext.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->ext.offer && !self->is_own_offer)
		receive_data(self);

	ext_data_control_offer_v1_destroy(offer);
	self->ext.offer = NULL;
	self->is_own_offer = false;
}

static const struct zwlr_data_control_device_v1_listener wlr_data_control_device_listener = {
	.data_offer = data_control_device_offer_wlr,
	.selection = data_control_device_selection_wlr,
	.finished = data_control_device_finished_wlr,
	.primary_selection = data_control_device_primary_selection_wlr
};

static const struct ext_data_control_device_v1_listener ext_data_control_device_listener = {
	.data_offer = data_control_device_offer_ext,
	.selection = data_control_device_selection_ext,
	.finished = data_control_device_finished_ext,
	.primary_selection = data_control_device_primary_selection_ext
};

static void data_control_source_send_handle(struct data_control* self,
		const char* mime_type, int32_t fd)
{
	const char* d = self->cb_data;
	size_t len = self->cb_len;
	int ret;

	assert(d);
	assert(len);

	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		d = custom_mime_type_data;
		len = strlen(custom_mime_type_data);
	}

	if (dont_block(fd) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to set O_NONBLOCK on clipbooard send fd");
		close(fd);
		return;
	}

	ret = write(fd, d, len);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ret = 0;
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Clipboard write failed: %m");
			close(fd);
			return;
		}
	} else if (ret == (int)len) {
		close(fd);
		return;
	}

	/* we did a partial write, so continue sending data asynchronously */

	struct send_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		goto ctx_alloc_failure;
		return;
	}

	ctx->fd = fd;
	ctx->length = len - ret;
	ctx->index = 0;
	ctx->data = malloc(ctx->length);
	if (!ctx->data) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		goto ctx_data_alloc_failure;
	}
	memcpy(ctx->data, d + ret, ctx->length);

	ctx->handler = aml_handler_new(ctx->fd, on_send, ctx, NULL);
	if (!ctx->handler)
		goto handler_failure;

	aml_set_event_mask(ctx->handler, AML_EVENT_WRITE);

	if (aml_start(aml_get_default(), ctx->handler) < 0)
		goto poll_start_failure;

	LIST_INSERT_HEAD(&self->send_contexts, ctx, link);
	return;

poll_start_failure:
	aml_unref(ctx->handler);
handler_failure:
	free(ctx->data);
ctx_data_alloc_failure:
	free(ctx);
ctx_alloc_failure:
	close(fd);
	nvnc_log(NVNC_LOG_ERROR, "Clipboard write incomplete");
}

static void data_control_source_send_wlr(void* data,
	struct zwlr_data_control_source_v1* source,
	const char* mime_type, int32_t fd)
{
	data_control_source_send_handle(data, mime_type, fd);
}

static void data_control_source_send_ext(void* data,
	struct ext_data_control_source_v1* source,
	const char* mime_type, int32_t fd)
{
	data_control_source_send_handle(data, mime_type, fd);
}

static void data_control_source_cancelled_wlr(void* data,
	struct zwlr_data_control_source_v1* source)
{
	struct data_control* self = data;

	if (self->wlr.selection == source) {
		self->wlr.selection = NULL;
	}
	if (self->wlr.primary_selection == source) {
		self->wlr.primary_selection = NULL;
	}
	zwlr_data_control_source_v1_destroy(source);
}

static void data_control_source_cancelled_ext(void* data,
	struct ext_data_control_source_v1* source)
{
	struct data_control* self = data;

	if (self->ext.selection == source) {
		self->ext.selection = NULL;
	}
	if (self->ext.primary_selection == source) {
		self->ext.primary_selection = NULL;
	}
	ext_data_control_source_v1_destroy(source);
}

static const struct zwlr_data_control_source_v1_listener wlr_data_control_source_listener = {
	.send = data_control_source_send_wlr,
	.cancelled = data_control_source_cancelled_wlr
};

static const struct ext_data_control_source_v1_listener ext_data_control_source_listener = {
	.send = data_control_source_send_ext,
	.cancelled = data_control_source_cancelled_ext
};

static bool set_selection(struct data_control* self, bool primary)
{
	if (data_control_is_wlr(self)) {
		struct zwlr_data_control_source_v1* selection =
			zwlr_data_control_manager_v1_create_data_source(self->wlr.manager);
		if (!selection) {
			nvnc_log(NVNC_LOG_ERROR,
				"zwlr_data_control_manager_v1_create_data_source() failed");
			free(self->cb_data);
			self->cb_data = NULL;
			return false;
		}

		zwlr_data_control_source_v1_add_listener(
			selection, &wlr_data_control_source_listener, self);
		zwlr_data_control_source_v1_offer(selection, self->mime_type);
		zwlr_data_control_source_v1_offer(selection, self->custom_mime_type_name);

		if (primary) {
			zwlr_data_control_device_v1_set_primary_selection(
				self->wlr.device, selection);
			self->wlr.primary_selection = selection;
		} else {
			zwlr_data_control_device_v1_set_selection(
				self->wlr.device, selection);
			self->wlr.selection = selection;
		}

		return true;
	}

	struct ext_data_control_source_v1* selection =
		ext_data_control_manager_v1_create_data_source(self->ext.manager);
	if (!selection) {
		nvnc_log(NVNC_LOG_ERROR,
			"ext_data_control_manager_v1_create_data_source() failed");
		free(self->cb_data);
		self->cb_data = NULL;
		return false;
	}

	ext_data_control_source_v1_add_listener(
		selection, &ext_data_control_source_listener, self);
	ext_data_control_source_v1_offer(selection, self->mime_type);
	ext_data_control_source_v1_offer(selection, self->custom_mime_type_name);

	if (primary) {
		ext_data_control_device_v1_set_primary_selection(
			self->ext.device, selection);
		self->ext.primary_selection = selection;
	} else {
		ext_data_control_device_v1_set_selection(
			self->ext.device, selection);
		self->ext.selection = selection;
	}

	return true;
}

void data_control_init(struct data_control* self, enum data_control_protocol protocol,
		struct zwlr_data_control_manager_v1* wlr_manager,
		struct ext_data_control_manager_v1* ext_manager,
		struct nvnc* server, struct wl_seat* seat)
{
	self->protocol = protocol;
	self->server = server;
	LIST_INIT(&self->receive_contexts);
	LIST_INIT(&self->send_contexts);
	self->wlr.manager = NULL;
	self->wlr.device = NULL;
	self->wlr.selection = NULL;
	self->wlr.primary_selection = NULL;
	self->wlr.offer = NULL;
	self->ext.manager = NULL;
	self->ext.device = NULL;
	self->ext.selection = NULL;
	self->ext.primary_selection = NULL;
	self->ext.offer = NULL;
	if (data_control_is_wlr(self)) {
		self->wlr.manager = wlr_manager;
		self->wlr.device = zwlr_data_control_manager_v1_get_data_device(
			self->wlr.manager, seat);
		zwlr_data_control_device_v1_add_listener(
			self->wlr.device, &wlr_data_control_device_listener, self);
	} else {
		self->ext.manager = ext_manager;
		self->ext.device = ext_data_control_manager_v1_get_data_device(
			self->ext.manager, seat);
		ext_data_control_device_v1_add_listener(
			self->ext.device, &ext_data_control_device_listener, self);
	}
	self->is_own_offer = false;
	self->cb_data = NULL;
	self->cb_len = 0;
	self->mime_type = "text/plain;charset=utf-8";
	snprintf(self->custom_mime_type_name,
			sizeof(self->custom_mime_type_name),
			"x-wayvnc-client-%08x", (unsigned int)rand());
}

void data_control_destroy(struct data_control* self)
{
	while (!LIST_EMPTY(&self->receive_contexts))
		destroy_receive_context(LIST_FIRST(&self->receive_contexts));
	while (!LIST_EMPTY(&self->send_contexts)) {
		nvnc_log(NVNC_LOG_ERROR, "Clipboard write incomplete due to client disconnection");
		destroy_send_context(LIST_FIRST(&self->send_contexts));
	}
	if (data_control_is_wlr(self)) {
		if (self->wlr.selection) {
			zwlr_data_control_source_v1_destroy(self->wlr.selection);
			self->wlr.selection = NULL;
		}
		if (self->wlr.primary_selection) {
			zwlr_data_control_source_v1_destroy(self->wlr.primary_selection);
			self->wlr.primary_selection = NULL;
		}
		if (self->wlr.device) {
			zwlr_data_control_device_v1_destroy(self->wlr.device);
			self->wlr.device = NULL;
		}
	} else {
		if (self->ext.selection) {
			ext_data_control_source_v1_destroy(self->ext.selection);
			self->ext.selection = NULL;
		}
		if (self->ext.primary_selection) {
			ext_data_control_source_v1_destroy(self->ext.primary_selection);
			self->ext.primary_selection = NULL;
		}
		if (self->ext.device) {
			ext_data_control_device_v1_destroy(self->ext.device);
			self->ext.device = NULL;
		}
	}
	free(self->cb_data);
}

void data_control_to_clipboard(struct data_control* self, const char* text, size_t len)
{
	if (!len) {
		nvnc_log(NVNC_LOG_DEBUG, "Ignoring empty clipboard from VNC client");
		return;
	}
	free(self->cb_data);

	self->cb_data = malloc(len);
	if (!self->cb_data) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		return;
	}

	memcpy(self->cb_data, text, len);
	self->cb_len = len;
	// Set copy/paste buffer
	if (!set_selection(self, false))
		return;
	// Set highlight/middle_click buffer
	if (!set_selection(self, true))
		return;
}
