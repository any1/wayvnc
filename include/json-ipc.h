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

#pragma once

#include <jansson.h>

struct jsonipc_request {
	const char* method;
	json_t* params;
	json_t* id;

	json_t* json;
};

#define IPC_CODE_SUCCESS 0

struct jsonipc_error {
	int code;
	json_t* data;
};

#define JSONIPC_ERR_INIT {0,NULL}

struct jsonipc_response {
	int code;
	json_t* data;
	json_t* id;

	json_t* json;
};

void jsonipc_error_set_new(struct jsonipc_error*, int code, json_t* data);
void jsonipc_error_printf(struct jsonipc_error*, int code, const char* fmt, ...);
void jsonipc_error_set_from_errno(struct jsonipc_error*, const char* context);
void jsonipc_error_cleanup(struct jsonipc_error*);

struct jsonipc_request* jsonipc_request_parse_new(json_t* root,
		struct jsonipc_error* err);
struct jsonipc_request* jsonipc_request_new(const char* method, json_t* params);
struct jsonipc_request* jsonipc_event_new(const char* method, json_t* params);
json_t* jsonipc_request_pack(struct jsonipc_request*, json_error_t* err);
void jsonipc_request_destroy(struct jsonipc_request*);

struct jsonipc_response* jsonipc_response_parse_new(json_t* root,
		struct jsonipc_error* err);
struct jsonipc_response* jsonipc_response_new(int code, json_t* data,
		json_t* id);
struct jsonipc_response* jsonipc_error_response_new(struct jsonipc_error* err,
		json_t* id);
void jsonipc_response_destroy(struct jsonipc_response*);
json_t* jsonipc_response_pack(struct jsonipc_response*, json_error_t* err);

json_t* jprintf(const char* fmt, ...);
json_t* jvprintf(const char* fmt, va_list ap);
