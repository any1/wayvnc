/*
 * Copyright (c) 2022 Jim Ramsay
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

#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "json-ipc.h"

static const char* jsonipc_id_key = "id";
static const char* jsonipc_method_key = "method";
static const char* jsonipc_params_key = "params";
static const char* jsonipc_code_key = "code";
static const char* jsonipc_data_key = "data";

void jsonipc_error_set_new(struct jsonipc_error* err, int code, json_t* data)
{
	if (!err)
		return;
	err->code = code;
	err->data = data;
}

void jsonipc_error_printf(struct jsonipc_error* err, int code, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	jsonipc_error_set_new(err, code, json_pack("{s:o}", "error",
				jvprintf(fmt, ap)));
	va_end(ap);
}

void jsonipc_error_set_from_errno(struct jsonipc_error* err,
		const char* context)
{
	jsonipc_error_printf(err, errno, "%s: %m", context);
}

void jsonipc_error_cleanup(struct jsonipc_error* err)
{
	if (!err)
		return;
	json_decref(err->data);
}

inline static bool is_valid_id(json_t* id)
{
	return id == NULL ||
		json_is_string(id) || json_is_number(id);
}

struct jsonipc_request* jsonipc_request_parse_new(json_t* root,
		struct jsonipc_error* err)
{
	struct jsonipc_request* ipc = calloc(1, sizeof(*ipc));
	ipc->json = root;
	json_incref(ipc->json);
	json_error_t unpack_error;
	if (json_unpack_ex(root, &unpack_error, 0, "{s:s, s?O, s?O}",
				jsonipc_method_key, &ipc->method,
				jsonipc_params_key, &ipc->params,
				jsonipc_id_key, &ipc->id) == -1) {
		jsonipc_error_printf(err, EINVAL, unpack_error.text);
		goto failure;
	}
	if (!is_valid_id(ipc->id)) {
		char* id = json_dumps(ipc->id, JSON_EMBED | JSON_ENCODE_ANY);
		jsonipc_error_printf(err, EINVAL,
				"Invalid ID \"%s\"", id);
		free(id);
		goto failure;
	}
	return ipc;

failure:
	jsonipc_request_destroy(ipc);
	return NULL;
}

static int request_id = 1;
struct jsonipc_request* jsonipc_request_new(const char* method, json_t* params)
{
	struct jsonipc_request* ipc = calloc(1, sizeof(*ipc));
	ipc->method = method;
	ipc->params = params;
	json_incref(ipc->params);
	ipc->id = json_integer(request_id++);
	return ipc;
}

json_t* jsonipc_request_pack(struct jsonipc_request* self, json_error_t* err)
{
	return json_pack_ex(err, 0, "{s:s, s:O*, s:O*}",
			jsonipc_method_key, self->method,
			jsonipc_params_key, self->params,
			jsonipc_id_key, self->id);
}

void jsonipc_request_destroy(struct jsonipc_request* self)
{
	json_decref(self->params);
	json_decref(self->id);
	json_decref(self->json);
	free(self);
}

struct jsonipc_response* jsonipc_response_parse_new(json_t* root,
		struct jsonipc_error* err)
{
	struct jsonipc_response* ipc = calloc(1, sizeof(*ipc));
	ipc->json = root;
	json_incref(ipc->json);
	json_error_t unpack_error;
	if (json_unpack_ex(root, &unpack_error, 0, "{s:i, s?O, s?O}",
				jsonipc_code_key, &ipc->code,
				jsonipc_data_key, &ipc->data,
				jsonipc_id_key, &ipc->id) == -1) {
		jsonipc_error_printf(err, EINVAL, unpack_error.text);
		goto failure;
	}
	if (!is_valid_id(ipc->id)) {
		char* id = json_dumps(ipc->id, JSON_EMBED | JSON_ENCODE_ANY);
		jsonipc_error_printf(err, EINVAL,
				"Invalid ID \"%s\"", id);
		free(id);
		goto failure;
	}
	return ipc;

failure:
	jsonipc_response_destroy(ipc);
	return NULL;
}

struct jsonipc_response* jsonipc_response_new(int code,
		json_t* data, json_t* id)
{
	struct jsonipc_response* rsp = calloc(1, sizeof(*rsp));
	rsp->code = code;
	json_incref(id);
	rsp->id = id;
	json_incref(data);
	rsp->data = data;
	return rsp;
}

struct jsonipc_response* jsonipc_error_response_new(
		struct jsonipc_error* err,
		json_t* id)
{
	return jsonipc_response_new(err->code, err->data, id);
}

void jsonipc_response_destroy(struct jsonipc_response* self)
{
	json_decref(self->data);
	json_decref(self->json);
	json_decref(self->id);
	free(self);
}

json_t* jsonipc_response_pack(struct jsonipc_response* self, json_error_t* err)
{
	return json_pack_ex(err, 0, "{s:i, s:O*, s:O*}",
			jsonipc_code_key, self->code,
			jsonipc_id_key, self->id,
			jsonipc_data_key, self->data);
}

json_t* jprintf(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	json_t* result = jvprintf(fmt, args);
	va_end(args);
	return result;
}

json_t* jvprintf(const char* fmt, va_list ap)
{
	char buffer[128];
	int len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	return json_stringn(buffer, len);
}
