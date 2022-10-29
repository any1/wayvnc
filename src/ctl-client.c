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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <jansson.h>

#include "json-ipc.h"
#include "ctl-client.h"
#include "ctl-server.h"
#include "strlcpy.h"
#include "util.h"

#define WARN(fmt, ...) \
	fprintf(stderr, "[WARNING] " fmt "\n", ##__VA_ARGS__)

static bool do_debug = false;

#define DEBUG(fmt, ...) \
	if (do_debug) \
		fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);

#define FAILED_TO(action) \
	WARN("Failed to " action ": %m");

struct ctl_client {
	void* userdata;

	char read_buffer[512];
	size_t read_len;

	int fd;
};

void ctl_client_debug_log(bool enable)
{
	do_debug = enable;
}

struct ctl_client* ctl_client_new(const char* socket_path, void* userdata)
{
	if (!socket_path)
		socket_path = default_ctl_socket_path();
	struct ctl_client* new = calloc(1, sizeof(*new));
	new->userdata = userdata;

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};

	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		FAILED_TO("create unix socket");
		goto socket_failure;
	}
	strcpy(addr.sun_path, socket_path);

	new->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (new->fd < 0) {
		FAILED_TO("create unix socket");
		goto socket_failure;
	}

	if (connect(new->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		FAILED_TO("connect to unix socket");
		goto connect_failure;
	}

	return new;

connect_failure:
	close(new->fd);
socket_failure:
	free(new);
	return NULL;
}

void ctl_client_destroy(struct ctl_client* self)
{
	close(self->fd);
	free(self);
}

void* ctl_client_userdata(struct ctl_client* self)
{
	return self->userdata;
}

static struct jsonipc_request*	ctl_client_parse_args(struct ctl_client* self,
		int argc, char* argv[])
{
	struct jsonipc_request* request = NULL;
	const char* method = argv[0];
	json_t* params = json_object();
	for (int i = 1; i < argc; ++i) {
		char* key = argv[i];
		char* value = NULL;
		if (key[0] == '-' && key[1] == '-')
			key += 2;
		char* delim = strchr(key, '=');
		if (delim) {
			*delim = '\0';
			value = delim + 1;
		} else if (++i < argc) {
			value = argv[i];
		} else {
			WARN("Argument must be of the format --key=value or --key value");
			goto failure;
		}
		json_object_set_new(params, key, json_string(value));
	}
	request = jsonipc_request_new(method, params);

failure:
	json_decref(params);
	return request;
}

static ssize_t ctl_client_send_request(struct ctl_client* self,
		struct jsonipc_request* request)
{
	json_error_t err;
	json_t* packed = jsonipc_request_pack(request, &err);
	if (!packed) {
		WARN("Could not encode json: %s", err.text);
		return -1;
	}
	char buffer[512];
	int len = json_dumpb(packed, buffer, sizeof(buffer), JSON_COMPACT);
	json_decref(packed);
	DEBUG(">> %.*s", len, buffer);
	return send(self->fd, buffer, len, MSG_NOSIGNAL);
}

static json_t* json_from_buffer(struct ctl_client* self)
{
	if (self->read_len == 0) {
		DEBUG("Read buffer is empty");
		errno = ENODATA;
		return NULL;
	}
	json_error_t err;
	json_t* root = json_loadb(self->read_buffer, self->read_len, 0, &err);
	if (root) {
		advance_read_buffer(&self->read_buffer, &self->read_len, err.position);
	} else if (json_error_code(&err) == json_error_premature_end_of_input) {
		DEBUG("Awaiting more data");
		errno = ENODATA;
	} else {
		WARN("Json parsing failed: %s", err.text);
		errno = EINVAL;
	}
	return root;
}

static json_t* read_one_object(struct ctl_client* self, int timeout_ms)
{
	json_t* root = json_from_buffer(self);
	if (root)
		return root;
	if (errno != ENODATA)
		return NULL;
	struct pollfd pfd = {
		.fd = self->fd,
		.events = POLLIN,
		.revents = 0
	};
	while (root == NULL) {
		int n = poll(&pfd, 1, timeout_ms);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			WARN("Error waiting for a response: %m");
			break;
		} else if (n == 0) {
			WARN("Timeout waiting for a response");
			break;
		}
		char* readptr = self->read_buffer + self->read_len;
		size_t remainder = sizeof(self->read_buffer) - self->read_len;
		n = recv(self->fd, readptr, remainder, 0);
		if (n == -1) {
			WARN("Read failed: %m");
			break;
		} else if (n == 0) {
			WARN("Disconnected");
			errno = ECONNRESET;
			break;
		}
		DEBUG("Read %d bytes", n);
		DEBUG("<< %.*s", n, readptr);
		self->read_len += n;
		root = json_from_buffer(self);
		if (!root && errno != ENODATA)
			break;
	}
	return root;
}

static struct jsonipc_response* ctl_client_wait_for_response(struct ctl_client* self)
{
	DEBUG("Waiting for a response");
	json_t* root = read_one_object(self, 1000);
	if (!root)
		return NULL;
	struct jsonipc_error jipc_err = JSONIPC_ERR_INIT;
	struct jsonipc_response* response = jsonipc_response_parse_new(root,
			&jipc_err);
	if (!response) {
		char* msg = json_dumps(jipc_err.data, JSON_EMBED);
		WARN("Could not parse json: %s", msg);
		free(msg);
	}
	json_decref(root);
	jsonipc_error_cleanup(&jipc_err);
	return response;
}

static int ctl_client_print_response(struct ctl_client* self,
		struct jsonipc_response* response)
{
	DEBUG("Response code: %d", response->code);
	if (response->data) {
		char* data = json_dumps(response->data, JSON_INDENT(4));
		printf("%s\n", data);
		free(data);
	}
	return response->code;
}

int ctl_client_run_command(struct ctl_client* self,
		int argc, char* argv[])
{
	int result = -1;
	struct jsonipc_request*	request = ctl_client_parse_args(self, argc, argv);
	if (!request)
		goto parse_failure;

	if (ctl_client_send_request(self, request) < 0)
		goto send_failure;

	struct jsonipc_response* response = ctl_client_wait_for_response(self);
	if (!response)
		goto receive_failure;

	result = ctl_client_print_response(self, response);

	jsonipc_response_destroy(response);
receive_failure:
send_failure:
	jsonipc_request_destroy(request);
parse_failure:
	return result;
}
