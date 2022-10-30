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

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <neatvnc.h>
#include <aml.h>
#include <jansson.h>

#include "output.h"
#include "ctl-server.h"
#include "json-ipc.h"
#include "util.h"
#include "strlcpy.h"

#define FAILED_TO(action) \
	nvnc_log(NVNC_LOG_WARNING, "Failed to " action ": %m");

enum cmd_type {
	CMD_HELP,
	CMD_SET_OUTPUT,
	CMD_UNKNOWN,
};

struct cmd_param_info {
	char* name;
	char* description;
};

struct cmd_info {
	char* name;
	char* description;
	struct cmd_param_info params[5];
};

static struct cmd_info cmd_list[] = {
	[CMD_HELP] = { "help",
		"List all commands, or show usage of a specific command",
		{
			{"command", "The command to show (optional)"},
			{NULL, NULL},
		}
	},
	[CMD_SET_OUTPUT] = { "set-output",
		"Switch the actively captured output",
		{
			{"switch-to", "The specific output name to capture"},
			{"cycle", "Either \"next\" or \"prev\""},
			{NULL, NULL},
		}
	},
};

struct cmd {
	enum cmd_type type;
};

struct cmd_help {
	struct cmd cmd;
	char command[64];
};

struct cmd_set_output {
	struct cmd cmd;
	char target[64];
	enum output_cycle_direction cycle;
};

struct cmd_response {
	int code;
	json_t* data;
};

struct ctl_client {
	int fd;
	struct wl_list link;
	struct ctl* server;
	struct aml_handler* handler;
	char read_buffer[512];
	size_t read_len;
	json_t* response_queue;
	char* write_buffer;
	char* write_ptr;
	size_t write_len;
	bool drop_after_next_send;
};

struct ctl {
	char socket_path[255];
	struct ctl_server_actions actions;
	int fd;
	struct aml_handler* handler;
	struct wl_list clients;
};

static struct cmd_response* cmd_response_new(int code, json_t* data)
{
	struct cmd_response* new = calloc(1, sizeof(struct cmd_response));
	new->code = code;
	new->data = data;
	return new;
}

static void cmd_response_destroy(struct cmd_response* self)
{
	json_decref(self->data);
	free(self);
}

static enum cmd_type parse_command_name(const char* name)
{
	if (!name || name[0] == '\0')
		return CMD_UNKNOWN;
	for (int i = 0; i < CMD_UNKNOWN; ++i) {
		if (strcmp(name, cmd_list[i].name) == 0) {
			return i;
		}
	}
	return CMD_UNKNOWN;
}

static struct cmd_help* cmd_help_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* command = NULL;
	if (args && json_unpack(args, "{s?s}", "command", &command) == -1) {
		jsonipc_error_printf(err, EINVAL,
				"expecting \"command\" (optional)");
		return NULL;
	}
	struct cmd_help* cmd = calloc(1, sizeof(*cmd));
	cmd->cmd.type = CMD_HELP;
	if (command)
		strlcpy(cmd->command, command, sizeof(cmd->command));
	return cmd;
}

static struct cmd_set_output* cmd_set_output_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* target = NULL;
	const char* cycle = NULL;
	if (json_unpack(args, "{s?s,s?s}",
			"switch-to", &target,
			"cycle", &cycle) == -1) {
		jsonipc_error_printf(err, EINVAL,
				"expecting \"switch-to\" or \"cycle\"");
		return NULL;
	}
	if ((!target && !cycle) || (target && cycle)) {
		jsonipc_error_printf(err, EINVAL,
				"expecting exactly one of \"switch-to\" or \"cycle\"");
		return NULL;
	}
	struct cmd_set_output* cmd = calloc(1, sizeof(*cmd));
	cmd->cmd.type = CMD_SET_OUTPUT;
	if (target) {
		strlcpy(cmd->target, target, sizeof(cmd->target));
	} else if (cycle) {
		if (strncmp(cycle, "prev", 4) == 0)
			cmd->cycle = OUTPUT_CYCLE_REVERSE;
		else if (strcmp(cycle, "next") == 0)
			cmd->cycle = OUTPUT_CYCLE_FORWARD;
		else {
			jsonipc_error_printf(err, EINVAL,
				"cycle must either be \"next\" or \"prev\"");
			free(cmd);
			return NULL;
		}
	}
	return cmd;
}

static json_t* list_allowed_commands()
{
	json_t* allowed = json_array();
	for (int i = 0; i < CMD_UNKNOWN; ++i)
		json_array_append_new(allowed, json_string(cmd_list[i].name));
	return allowed;
}

static struct cmd* parse_command(struct jsonipc_request* ipc,
		struct jsonipc_error* err)
{
	nvnc_trace("Parsing command %s", ipc->method);
	enum cmd_type cmd_type = parse_command_name(ipc->method);
	struct cmd* cmd = NULL;
	switch (cmd_type) {
	case CMD_HELP:
		cmd = (struct cmd*)cmd_help_new(ipc->params, err);
		break;
	case CMD_SET_OUTPUT:
		cmd = (struct cmd*)cmd_set_output_new(ipc->params, err);
		break;
	case CMD_UNKNOWN:
		jsonipc_error_set_new(err, ENOENT,
				json_pack("{s:o, s:o}",
					"error",
					jprintf("Unknown command \"%s\"",
						ipc->method),
					"commands", list_allowed_commands()));
	}
	return cmd;
}

static void client_destroy(struct ctl_client* self)
{
	nvnc_trace("Destroying client %p", self);
	aml_stop(aml_get_default(), self->handler);
	aml_unref(self->handler);
	close(self->fd);
	json_array_clear(self->response_queue);
	json_decref(self->response_queue);
	wl_list_remove(&self->link);
	free(self);
}
static void set_internal_error(struct cmd_response** err, int code,
		const char* fmt, ...)
{
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	nvnc_log(NVNC_LOG_WARNING, msg);
	*err = cmd_response_new(code, json_pack("{s:s}", "error", msg));
}

// Return values:
// >0: Number of bytes read
// 0: No bytes read (EAGAIN)
// -1: Fatal error.  Check 'err' for details, or if 'err' is null, terminate the connection.
static ssize_t client_read(struct ctl_client* self, struct cmd_response** err)
{
	size_t bufferspace = sizeof(self->read_buffer) - self->read_len;
	if (bufferspace == 0) {
		set_internal_error(err, EIO, "Buffer overflow");
		return -1;
	}
	ssize_t n = recv(self->fd, self->read_buffer + self->read_len, bufferspace,
			MSG_DONTWAIT);
	if (n == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			nvnc_trace("recv: EAGAIN");
			return 0;
		}
		set_internal_error(err, EIO, "Read failed: %m");
		return -1;
	} else if (n == 0) {
		nvnc_log(NVNC_LOG_INFO, "Control socket client disconnected: %p", self);
		errno = ENOTCONN;
		return -1;
	}
	self->read_len += n;
	nvnc_trace("Read %d bytes, total is now %d", n, self->read_len);
	return n;
}

static json_t* client_next_object(struct ctl_client* self, struct cmd_response** ierr)
{
	if (self->read_len == 0)
		return NULL;

	json_error_t err;
	json_t* root = json_loadb(self->read_buffer, self->read_len,
			JSON_DISABLE_EOF_CHECK, &err);
	if (root) {
		nvnc_log(NVNC_LOG_DEBUG, "<< %.*s", err.position, self->read_buffer);
		advance_read_buffer(&self->read_buffer, &self->read_len, err.position);
	} else if (json_error_code(&err) == json_error_premature_end_of_input) {
		nvnc_trace("Awaiting more data");
	} else {
		set_internal_error(ierr, EINVAL, err.text);
	}
	return root;
}

static struct cmd_response* generate_help_object(const char* cmd)
{
	enum cmd_type ctype = parse_command_name(cmd);
	json_t* data;
	if (ctype == CMD_UNKNOWN) {
		data = json_pack("{s:o}", "commands", list_allowed_commands());
	} else {
		struct cmd_info* info = &cmd_list[ctype];
		json_t* param_list = NULL;
		if (info->params[0].name) {
			param_list = json_object();
			for (struct cmd_param_info* param = info->params;
					param->name; ++param)
				json_object_set_new(param_list, param->name,
						json_string(param->description));
		}
		data = json_pack("{s:{s:s, s:o*}}",
				info->name,
				"description", info->description,
				"params", param_list);
	}
	struct cmd_response* response = cmd_ok();
	response->data = data;
	return response;
}

static struct cmd_response* ctl_server_dispatch_cmd(struct ctl* self, struct cmd* cmd)
{
	assert(cmd->type != CMD_UNKNOWN);
	const struct cmd_info* info = &cmd_list[cmd->type];
	nvnc_log(NVNC_LOG_INFO, "Dispatching control client command '%s'", info->name);
	struct cmd_response* response = NULL;
	switch (cmd->type) {
	case CMD_HELP:{
		struct cmd_help* c = (struct cmd_help*)cmd;
		response = generate_help_object(c->command);
		break;
		}
	case CMD_SET_OUTPUT: {
		struct cmd_set_output* c = (struct cmd_set_output*)cmd;
		if (c->target[0] != '\0')
			response = self->actions.on_output_switch(self, c->target);
		else
			response = self->actions.on_output_cycle(self, c->cycle);
		break;
		}
	case CMD_UNKNOWN:
		break;
	}
	return response;
}

static void client_set_aml_event_mask(struct ctl_client* self)
{
	int mask = AML_EVENT_READ;
	if (json_array_size(self->response_queue) > 0 ||
			self->write_len)
		mask |= AML_EVENT_WRITE;
	aml_set_event_mask(self->handler, mask);
}

static int client_enqueue_jsonipc(struct ctl_client* self,
		struct jsonipc_response* resp)
{
	int result = 0;
	json_error_t err;
	json_t* packed_response = jsonipc_response_pack(resp, &err);
	if (!packed_response) {
		nvnc_log(NVNC_LOG_WARNING, "Pack failed: %s", err.text);
		result = -1;
		goto failure;
	}
	result = json_array_append_new(self->response_queue, packed_response);
	if (result != 0) {
		nvnc_log(NVNC_LOG_WARNING, "Append failed");
		goto failure;
	}
	client_set_aml_event_mask(self);
failure:
	jsonipc_response_destroy(resp);
	return result;
}

static int client_enqueue_error(struct ctl_client* self,
		struct jsonipc_error* err, json_t* id)
{
	struct jsonipc_response* resp = jsonipc_error_response_new(err, id);
	return client_enqueue_jsonipc(self, resp);
}

static int client_enqueue_response(struct ctl_client* self,
		struct cmd_response* response, json_t* id)
{
	nvnc_log(NVNC_LOG_INFO, "Enqueueing response: %s (%d)",
			response->code == 0 ? "OK" : "FAILED", response->code);
	char* str = NULL;
	if (response->data)
		str = json_dumps(response->data, 0);
	nvnc_log(NVNC_LOG_DEBUG, "Response data: %s", str);
	if(str)
		free(str);
	struct jsonipc_response* resp =
		jsonipc_response_new(response->code, response->data, id);
	cmd_response_destroy(response);
	return client_enqueue_jsonipc(self, resp);
}

static int client_enqueue_internal_error(struct ctl_client* self,
		struct cmd_response* err)
{
	int result = client_enqueue_response(self, err, NULL);
	if (result != 0)
		client_destroy(self);
	self->drop_after_next_send = true;
	return result;
}

static void send_ready(struct ctl_client* client)
{
	if (client->write_buffer) {
		nvnc_trace("Continuing partial write (%d left)", client->write_len);
	} else if (json_array_size(client->response_queue) > 0){
		nvnc_trace("Sending new queued message");
		json_t* item = json_array_get(client->response_queue, 0);
		client->write_len = json_dumpb(item, NULL, 0, JSON_COMPACT);
		client->write_buffer = calloc(1, client->write_len);
		client->write_ptr = client->write_buffer;
		json_dumpb(item, client->write_buffer, client->write_len,
				JSON_COMPACT);
		nvnc_log(NVNC_LOG_DEBUG, ">> %.*s", client->write_len, client->write_buffer);
		json_array_remove(client->response_queue, 0);
	} else {
		nvnc_trace("Nothing to send");
	}
	if (!client->write_ptr)
		goto no_data;
	ssize_t n = send(client->fd, client->write_ptr, client->write_len,
			MSG_NOSIGNAL|MSG_DONTWAIT);
	if (n == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			nvnc_trace("send: EAGAIN");
			goto send_eagain;
		}
		nvnc_log(NVNC_LOG_ERROR, "Could not send response: %m");
		client_destroy(client);
		return;
	}
	nvnc_trace("sent %d/%d bytes", n, client->write_len);
	client->write_ptr += n;
	client->write_len -= n;
send_eagain:
	if (client->write_len == 0) {
		nvnc_trace("Write buffer empty!");
		free(client->write_buffer);
		client->write_buffer = NULL;
		client->write_ptr = NULL;
		if (client->drop_after_next_send) {
			nvnc_log(NVNC_LOG_WARNING, "Intentional disconnect");
			client_destroy(client);
			return;
		}
	} else {
		nvnc_trace("Write buffer has %d remaining", client->write_len);
	}
no_data:
	client_set_aml_event_mask(client);
}

static void recv_ready(struct ctl_client* client)
{
	struct ctl* server = client->server;
	struct cmd_response* details = NULL;
	switch (client_read(client, &details)) {
	case 0: // Needs more data
		return;
	case -1: // Fatal error
		if (details)
			client_enqueue_internal_error(client, details);
		else
			client_destroy(client);
		return;
	default: // Read some data; check it
		break;
	}

	json_t* root;
	while (true) {
		root = client_next_object(client, &details);
		if (root == NULL)
			break;

		struct jsonipc_error jipc_err = JSONIPC_ERR_INIT;

		struct jsonipc_request* request =
			jsonipc_request_parse_new(root, &jipc_err);
		if (!request) {
			client_enqueue_error(client, &jipc_err,
					NULL);
			goto request_parse_failed;
		}

		struct cmd* cmd = parse_command(request, &jipc_err);
		if (!cmd) {
			client_enqueue_error(client, &jipc_err,
					request->id);
			goto cmdparse_failed;
		}

		// TODO: Enqueue the command (and request ID) to be
		// handled by the main loop instead of doing the
		// dispatch here
		struct cmd_response* response =
			ctl_server_dispatch_cmd(server, cmd);
		if (!response)
			goto no_response;
		client_enqueue_response(client, response, request->id);
no_response:
		free(cmd);
cmdparse_failed:
		jsonipc_request_destroy(request);
request_parse_failed:
		jsonipc_error_cleanup(&jipc_err);
		json_decref(root);
	}
	if (details)
		client_enqueue_internal_error(client, details);
}

static void on_ready(void* obj)
{
	struct ctl_client* client = aml_get_userdata(obj);
	uint32_t events = aml_get_revents(obj);
	nvnc_trace("Client %p ready: 0x%x", client, events);

	if (events & AML_EVENT_WRITE)
		send_ready(client);
	else if (events & AML_EVENT_READ)
		recv_ready(client);
}

static void on_connection(void* obj)
{
	nvnc_log(NVNC_LOG_DEBUG, "New connection");
	struct ctl* server = aml_get_userdata(obj);

	struct ctl_client* client = calloc(1, sizeof(*client));
	if (!client) {
		FAILED_TO("allocate a client object");
		return;
	}

	client->server = server;
	client->response_queue = json_array();

	client->fd = accept(server->fd, NULL, 0);
	if (client->fd < 0) {
		FAILED_TO("accept a connection");
		goto accept_failure;
	}

	client->handler = aml_handler_new(client->fd, on_ready, client, NULL);
	if (!client->handler) {
		FAILED_TO("create a loop handler");
		goto handle_failure;
	}

	if (aml_start(aml_get_default(), client->handler) < 0) {
		FAILED_TO("register for client events");
		goto poll_start_failure;
	}

	wl_list_insert(&server->clients, &client->link);
	nvnc_log(NVNC_LOG_INFO, "New control socket client connected: %p", client);
	return;

poll_start_failure:
	aml_unref(client->handler);
handle_failure:
	close(client->fd);
accept_failure:
	json_decref(client->response_queue);
	free(client);
}

int ctl_server_init(struct ctl* self, const char* socket_path)
{
	if (!socket_path) {
		socket_path = default_ctl_socket_path();
		if (!getenv("XDG_RUNTIME_DIR"))
			nvnc_log(NVNC_LOG_WARNING, "$XDG_RUNTIME_DIR is not set. Falling back to control socket \"%s\"", socket_path);
	}
	strlcpy(self->socket_path, socket_path, sizeof(self->socket_path));
	nvnc_log(NVNC_LOG_DEBUG, "Initializing wayvncctl socket: %s", self->socket_path);

	wl_list_init(&self->clients);

	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};

	if (strlen(self->socket_path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		FAILED_TO("create unix socket");
		goto socket_failure;
	}
	strcpy(addr.sun_path, self->socket_path);

	self->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (self->fd < 0) {
		FAILED_TO("create unix socket");
		goto socket_failure;
	}

	if (bind(self->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		FAILED_TO("bind unix socket");
		goto bind_failure;
	}

	if (listen(self->fd, 16) < 0) {
		FAILED_TO("listen to unix socket");
		goto listen_failure;
	}

	self->handler = aml_handler_new(self->fd, on_connection, self, NULL);
	if (!self->handler) {
		FAILED_TO("create a main loop handler");
		goto handle_failure;
	}

	if (aml_start(aml_get_default(), self->handler) < 0) {
		FAILED_TO("Register for server events");
		goto poll_start_failure;
	}
	return 0;

poll_start_failure:
	aml_unref(self->handler);
handle_failure:
listen_failure:
	close(self->fd);
	unlink(self->socket_path);
bind_failure:
socket_failure:
	return -1;
}

static void ctl_server_stop(struct ctl* self)
{
	aml_stop(aml_get_default(), self->handler);
	aml_unref(self->handler);
	struct ctl_client* client;
	struct ctl_client* tmp;
	wl_list_for_each_safe(client, tmp, &self->clients, link)
		client_destroy(client);
	close(self->fd);
	unlink(self->socket_path);
}

struct ctl* ctl_server_new(const char* socket_path,
		const struct ctl_server_actions* actions)
{
	struct ctl* ctl = calloc(1, sizeof(*ctl));
	memcpy(&ctl->actions, actions, sizeof(*actions));
	if (ctl_server_init(ctl, socket_path) != 0) {
		free(ctl);
		return NULL;
	}
	return ctl;
}

void ctl_server_destroy(struct ctl* self)
{
	ctl_server_stop(self);
	free(self);
}

void* ctl_server_userdata(struct ctl* self)
{
	return self->actions.userdata;
}

struct cmd_response* cmd_ok()
{
	return cmd_response_new(0, NULL);
}

struct cmd_response* cmd_failed(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	struct cmd_response* resp = cmd_response_new(1, json_pack("{s:o}",
				"error", jvprintf(fmt, ap)));
	va_end(ap);
	return resp;
}
