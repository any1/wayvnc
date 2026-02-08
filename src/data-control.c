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

static void data_control_offer_receive(struct data_control* self, void* offer, int fd);
static void data_control_offer_destroy(struct data_control* self, void* offer);
static void data_control_offer_add_listener(struct data_control* self, void* offer);
static void data_control_source_destroy(struct data_control* self, void* source);
static void data_control_source_offer(struct data_control* self, void* source,
		const char* mime_type);
static void data_control_source_add_listener(struct data_control* self, void* source);
static void data_control_device_destroy(struct data_control* self, void* device);
static void data_control_device_set_selection(struct data_control* self, void* source);
static void data_control_device_set_primary_selection(struct data_control* self, void* source);
static void* data_control_manager_create_data_source(struct data_control* self);
static void* data_control_manager_get_data_device(struct data_control* self,
		struct wl_seat* seat);

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

static void receive_data(struct data_control* self, void* offer)
{
	int pipe_fd[2];

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

	data_control_offer_receive(self, offer, pipe_fd[1]);
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

static void data_control_offer_handle(struct data_control* self, void* offer,
		const char* mime_type)
{
	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		self->is_own_offer = true;
		return;
	}

	if (self->offer)
		return;

	if (strcmp(mime_type, self->mime_type) == 0)
		self->offer = offer;
}

static void data_control_offer_wlr(void* data,
	struct zwlr_data_control_offer_v1* offer,
	const char* mime_type)
{
	data_control_offer_handle(data, offer, mime_type);
}

static void data_control_offer_ext(void* data,
	struct ext_data_control_offer_v1* offer,
	const char* mime_type)
{
	data_control_offer_handle(data, offer, mime_type);
}

static const struct zwlr_data_control_offer_v1_listener wlr_data_control_offer_listener = {
	.offer = data_control_offer_wlr
};

static const struct ext_data_control_offer_v1_listener ext_data_control_offer_listener = {
	.offer = data_control_offer_ext
};

static void data_control_device_offer_handle(struct data_control* self, void* offer)
{
	if (!offer)
		return;

	data_control_offer_add_listener(self, offer);
}

static void data_control_device_selection_handle(struct data_control* self, void* offer)
{
	if (!offer) {
		if (self->offer) {
			data_control_offer_destroy(self, self->offer);
			self->offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->offer && !self->is_own_offer)
		receive_data(self, offer);

	data_control_offer_destroy(self, offer);
	self->offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_finished_handle(struct data_control* self,
		void* device)
{
	data_control_device_destroy(self, device);
}

static void data_control_device_primary_selection_handle(struct data_control* self,
		void* offer)
{
	if (!offer) {
		if (self->offer) {
			data_control_offer_destroy(self, self->offer);
			self->offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (offer == self->offer && !self->is_own_offer)
		receive_data(self, offer);

	data_control_offer_destroy(self, offer);
	self->offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_offer_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	data_control_device_offer_handle(data, offer);
}

static void data_control_device_offer_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	data_control_device_offer_handle(data, offer);
}

static void data_control_device_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	data_control_device_selection_handle(data, offer);
}

static void data_control_device_selection_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	data_control_device_selection_handle(data, offer);
}

static void data_control_device_finished_wlr(void* data,
	struct zwlr_data_control_device_v1* device)
{
	data_control_device_finished_handle(data, device);
}

static void data_control_device_finished_ext(void* data,
	struct ext_data_control_device_v1* device)
{
	data_control_device_finished_handle(data, device);
}

static void data_control_device_primary_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* device,
	struct zwlr_data_control_offer_v1* offer)
{
	data_control_device_primary_selection_handle(data, offer);
}

static void data_control_device_primary_selection_ext(void* data,
	struct ext_data_control_device_v1* device,
	struct ext_data_control_offer_v1* offer)
{
	data_control_device_primary_selection_handle(data, offer);
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

static void data_control_source_cancelled_handle(struct data_control* self,
		void* source)
{
	if (self->selection == source) {
		self->selection = NULL;
	}
	if (self->primary_selection == source) {
		self->primary_selection = NULL;
	}
	data_control_source_destroy(self, source);
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
	data_control_source_cancelled_handle(data, source);
}

static void data_control_source_cancelled_ext(void* data,
	struct ext_data_control_source_v1* source)
{
	data_control_source_cancelled_handle(data, source);
}

static const struct zwlr_data_control_source_v1_listener wlr_data_control_source_listener = {
	.send = data_control_source_send_wlr,
	.cancelled = data_control_source_cancelled_wlr
};

static const struct ext_data_control_source_v1_listener ext_data_control_source_listener = {
	.send = data_control_source_send_ext,
	.cancelled = data_control_source_cancelled_ext
};

static void data_control_offer_receive(struct data_control* self, void* offer, int fd)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_offer_v1_receive(
			(struct zwlr_data_control_offer_v1*)offer,
			self->mime_type, fd);
		return;
	}

	ext_data_control_offer_v1_receive(
		(struct ext_data_control_offer_v1*)offer,
		self->mime_type, fd);
}

static void data_control_offer_destroy(struct data_control* self, void* offer)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_offer_v1_destroy(
			(struct zwlr_data_control_offer_v1*)offer);
		return;
	}

	ext_data_control_offer_v1_destroy(
		(struct ext_data_control_offer_v1*)offer);
}

static void data_control_offer_add_listener(struct data_control* self, void* offer)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_offer_v1_add_listener(
			(struct zwlr_data_control_offer_v1*)offer,
			&wlr_data_control_offer_listener, self);
		return;
	}

	ext_data_control_offer_v1_add_listener(
		(struct ext_data_control_offer_v1*)offer,
		&ext_data_control_offer_listener, self);
}

static void data_control_source_destroy(struct data_control* self, void* source)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_source_v1_destroy(
			(struct zwlr_data_control_source_v1*)source);
		return;
	}

	ext_data_control_source_v1_destroy(
		(struct ext_data_control_source_v1*)source);
}

static void data_control_source_offer(struct data_control* self, void* source,
		const char* mime_type)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_source_v1_offer(
			(struct zwlr_data_control_source_v1*)source,
			mime_type);
		return;
	}

	ext_data_control_source_v1_offer(
		(struct ext_data_control_source_v1*)source,
		mime_type);
}

static void data_control_source_add_listener(struct data_control* self, void* source)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_source_v1_add_listener(
			(struct zwlr_data_control_source_v1*)source,
			&wlr_data_control_source_listener, self);
		return;
	}

	ext_data_control_source_v1_add_listener(
		(struct ext_data_control_source_v1*)source,
		&ext_data_control_source_listener, self);
}

static void data_control_device_destroy(struct data_control* self, void* device)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_device_v1_destroy(
			(struct zwlr_data_control_device_v1*)device);
		return;
	}

	ext_data_control_device_v1_destroy(
		(struct ext_data_control_device_v1*)device);
}

static void data_control_device_set_selection(struct data_control* self, void* source)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_device_v1_set_selection(
			(struct zwlr_data_control_device_v1*)self->device,
			(struct zwlr_data_control_source_v1*)source);
		return;
	}

	ext_data_control_device_v1_set_selection(
		(struct ext_data_control_device_v1*)self->device,
		(struct ext_data_control_source_v1*)source);
}

static void data_control_device_set_primary_selection(struct data_control* self, void* source)
{
	if (data_control_is_wlr(self)) {
		zwlr_data_control_device_v1_set_primary_selection(
			(struct zwlr_data_control_device_v1*)self->device,
			(struct zwlr_data_control_source_v1*)source);
		return;
	}

	ext_data_control_device_v1_set_primary_selection(
		(struct ext_data_control_device_v1*)self->device,
		(struct ext_data_control_source_v1*)source);
}

static void* data_control_manager_create_data_source(struct data_control* self)
{
	if (data_control_is_wlr(self))
		return zwlr_data_control_manager_v1_create_data_source(
			(struct zwlr_data_control_manager_v1*)self->manager);

	return ext_data_control_manager_v1_create_data_source(
		(struct ext_data_control_manager_v1*)self->manager);
}

static void* data_control_manager_get_data_device(struct data_control* self,
		struct wl_seat* seat)
{
	if (data_control_is_wlr(self))
		return zwlr_data_control_manager_v1_get_data_device(
			(struct zwlr_data_control_manager_v1*)self->manager,
			seat);

	return ext_data_control_manager_v1_get_data_device(
		(struct ext_data_control_manager_v1*)self->manager,
		seat);
}

static void* set_selection(struct data_control* self, bool primary)
{
	void* selection = data_control_manager_create_data_source(self);
	if (selection == NULL) {
		nvnc_log(NVNC_LOG_ERROR, "data_control_manager_create_data_source() failed");
		free(self->cb_data);
		self->cb_data = NULL;
		return NULL;
	}

	data_control_source_add_listener(self, selection);
	data_control_source_offer(self, selection, self->mime_type);
	data_control_source_offer(self, selection, self->custom_mime_type_name);

	if (primary)
		data_control_device_set_primary_selection(self, selection);
	else
		data_control_device_set_selection(self, selection);

	return selection;
}

void data_control_init(struct data_control* self, enum data_control_protocol protocol,
		void* manager, struct nvnc* server, struct wl_seat* seat)
{
	self->protocol = protocol;
	self->manager = manager;
	self->server = server;
	LIST_INIT(&self->receive_contexts);
	LIST_INIT(&self->send_contexts);
	self->device = data_control_manager_get_data_device(self, seat);
	if (data_control_is_wlr(self)) {
		zwlr_data_control_device_v1_add_listener(
			(struct zwlr_data_control_device_v1*)self->device,
			&wlr_data_control_device_listener, self);
	} else {
		ext_data_control_device_v1_add_listener(
			(struct ext_data_control_device_v1*)self->device,
			&ext_data_control_device_listener, self);
	}
	self->selection = NULL;
	self->primary_selection = NULL;
	self->offer = NULL;
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
	if (self->selection) {
		data_control_source_destroy(self, self->selection);
		self->selection = NULL;
	}
	if (self->primary_selection) {
		data_control_source_destroy(self, self->primary_selection);
		self->primary_selection = NULL;
	}
	if (self->device) {
		data_control_device_destroy(self, self->device);
		self->device = NULL;
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
	self->selection = set_selection(self, false);
	// Set highlight/middle_click buffer
	self->primary_selection = set_selection(self, true);
}
