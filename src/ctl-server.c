/*
 * Copyright (c) 2022-2023 Jim Ramsay
 * Copyright (c) 2023 Andri Yngvason
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
#include <sys/stat.h>
#include <netdb.h>
#include <neatvnc.h>
#include <aml.h>
#include <jansson.h>

#include "output.h"
#include "ctl-commands.h"
#include "ctl-server.h"
#include "json-ipc.h"
#include "util.h"
#include "strlcpy.h"

#define FAILED_TO(action) \
	nvnc_log(NVNC_LOG_ERROR, "Failed to " action ": %m");

enum send_priority {
	SEND_FIFO,
	SEND_IMMEDIATE,
};

struct cmd {
	enum cmd_type type;
};

struct cmd_attach {
	struct cmd cmd;
	char display[128];
};

struct cmd_help {
	struct cmd cmd;
	char id[64];
	bool id_is_command;
};

struct cmd_set_output {
	struct cmd cmd;
	char target[64];
	enum output_cycle_direction cycle;
};

struct cmd_disconnect_client {
	struct cmd cmd;
	char id[64];
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
	bool accept_events;
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

static struct cmd_attach* cmd_attach_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* display = NULL;
	if (json_unpack(args, "{s:s}", "display", &display) == -1) {
		jsonipc_error_printf(err, EINVAL, "Missing display name");
		return NULL;
	}
	struct cmd_attach* cmd = calloc(1, sizeof(*cmd));
	strlcpy(cmd->display, display, sizeof(cmd->display));
	return cmd;
}

static struct cmd_help* cmd_help_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* command = NULL;
	const char* event = NULL;
	if (args && json_unpack(args, "{s?s, s?s}",
				"command", &command,
				"event", &event) == -1) {
		jsonipc_error_printf(err, EINVAL,
				"expecting \"command\" or \"event\" (optional)");
		return NULL;
	}
	if (command && event) {
		jsonipc_error_printf(err, EINVAL,
				"expecting exacly one of \"command\" or \"event\"");
		return NULL;
	}
	struct cmd_help* cmd = calloc(1, sizeof(*cmd));
	if (command) {
		strlcpy(cmd->id, command, sizeof(cmd->id));
		cmd->id_is_command = true;
	} else if (event) {
		strlcpy(cmd->id, event, sizeof(cmd->id));
		cmd->id_is_command = false;
	}
	return cmd;
}

static struct cmd_set_output* cmd_set_output_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* target = NULL;
	if (json_unpack(args, "{s:s}", "output-name", &target) == -1) {
		jsonipc_error_printf(err, EINVAL, "Missing output name");
		return NULL;
	}
	struct cmd_set_output* cmd = calloc(1, sizeof(*cmd));
	strlcpy(cmd->target, target, sizeof(cmd->target));
	return cmd;
}

static struct cmd_disconnect_client* cmd_disconnect_client_new(json_t* args,
		struct jsonipc_error* err)
{
	const char* id = NULL;
	if (json_unpack(args, "{s:s}", "id", &id) == -1) {
		jsonipc_error_printf(err, EINVAL, "Missing client id");
		return NULL;
	}
	struct cmd_disconnect_client* cmd = calloc(1, sizeof(*cmd));
	strlcpy(cmd->id, id, sizeof(cmd->id));
	return cmd;
}

static json_t* list_allowed(struct cmd_info (*list)[], size_t len)
{
	json_t* allowed = json_array();
	for (size_t i = 0; i < len; ++i) {
		json_array_append_new(allowed, json_string((*list)[i].name));
	}
	return allowed;
}

static json_t* list_allowed_commands()
{
	return list_allowed(&ctl_command_list, CMD_LIST_LEN);
}

static json_t* list_allowed_events()
{
	return list_allowed(&ctl_event_list, EVT_LIST_LEN);
}

static struct cmd* parse_command(struct jsonipc_request* ipc,
		struct jsonipc_error* err)
{
	nvnc_trace("Parsing command %s", ipc->method);
	enum cmd_type cmd_type = ctl_command_parse_name(ipc->method);
	struct cmd* cmd = NULL;
	switch (cmd_type) {
	case CMD_ATTACH:
		cmd = (struct cmd*)cmd_attach_new(ipc->params, err);
		break;
	case CMD_HELP:
		cmd = (struct cmd*)cmd_help_new(ipc->params, err);
		break;
	case CMD_OUTPUT_SET:
		cmd = (struct cmd*)cmd_set_output_new(ipc->params, err);
		break;
	case CMD_CLIENT_DISCONNECT:
		cmd = (struct cmd*)cmd_disconnect_client_new(ipc->params, err);
		break;
	case CMD_DETACH:
	case CMD_VERSION:
	case CMD_EVENT_RECEIVE:
	case CMD_CLIENT_LIST:
	case CMD_OUTPUT_LIST:
	case CMD_OUTPUT_CYCLE:
	case CMD_WAYVNC_EXIT:
		cmd = calloc(1, sizeof(*cmd));
		break;
	case CMD_UNKNOWN:
		jsonipc_error_set_new(err, ENOENT,
				json_pack("{s:o, s:o}",
					"error",
					jprintf("Unknown command \"%s\"",
						ipc->method),
					"commands", list_allowed_commands()));
		break;
	}
	if (cmd)
		cmd->type = cmd_type;
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

static struct cmd_response* generate_help_object(const char* id, bool id_is_command)
{
	struct cmd_info* info = id_is_command ?
		ctl_command_by_name(id) :
		ctl_event_by_name(id);
	json_t* data;
	if (!info) {
		data = json_pack("{s:o, s:o}",
				"commands", list_allowed_commands(),
				"events", list_allowed_events());
	} else {
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

static struct cmd_response* generate_version_object()
{
	struct cmd_response* response = cmd_ok();
	response->data = json_pack("{s:s, s:s, s:s}",
			"wayvnc", wayvnc_version,
			"neatvnc", nvnc_version,
			"aml", aml_version);
	return response;
}

static struct ctl_server_client* ctl_server_client_first(struct ctl* self)
{
	return self->actions.client_next(self, NULL);
}

static struct ctl_server_client* ctl_server_client_next(struct ctl* self,
		struct ctl_server_client* prev)
{
	return self->actions.client_next(self, prev);
}

static void ctl_server_client_get_info(struct ctl* self,
		const struct ctl_server_client* client,
		struct ctl_server_client_info* info)
{
	return self->actions.client_info(client, info);
}

static struct cmd_response* generate_vnc_client_list(struct ctl* self)
{
	struct cmd_response* response = cmd_ok();
	response->data = json_array();

	struct ctl_server_client* client;
	for (client = ctl_server_client_first(self); client;
			client = ctl_server_client_next(self, client)) {
		struct ctl_server_client_info info = {};
		ctl_server_client_get_info(self, client, &info);

		char id_str[64];
		snprintf(id_str, sizeof(id_str), "%d", info.id);
		json_t* packed = json_pack("{s:s}", "id", id_str);

		if (info.hostname)
			json_object_set_new(packed, "hostname",
					json_string(info.hostname));

		if (info.username)
			json_object_set_new(packed, "username",
					json_string(info.username));

		if (info.seat)
			json_object_set_new(packed, "seat",
					json_string(info.seat));

		json_array_append_new(response->data, packed);
	}

	return response;
}

static struct cmd_response* generate_output_list(struct ctl* self)
{
	struct ctl_server_output* outputs;
	size_t num_outputs = self->actions.get_output_list(self, &outputs);
	struct cmd_response* response = cmd_ok();

	response->data = json_array();
	for (size_t i = 0; i < num_outputs; ++i)
		json_array_append_new(response->data, json_pack(
					"{s:s, s:s, s:i, s:i, s:b, s:s}",
				"name", outputs[i].name,
				"description", outputs[i].description,
				"height", outputs[i].height,
				"width", outputs[i].width,
				"captured", outputs[i].captured,
				"power", outputs[i].power));
	free(outputs);
	return response;
}

static struct cmd_response* ctl_server_dispatch_cmd(struct ctl* self,
		struct ctl_client* client, struct cmd* cmd)
{
	const struct cmd_info* info = ctl_command_by_type(cmd->type);
	assert(info);
	nvnc_log(NVNC_LOG_INFO, "Dispatching control client command '%s'", info->name);
	struct cmd_response* response = NULL;
	switch (cmd->type) {
	case CMD_ATTACH:{
		struct cmd_attach* c = (struct cmd_attach*)cmd;
		response = self->actions.on_attach(self, c->display);
		break;
		}
	case CMD_HELP:{
		struct cmd_help* c = (struct cmd_help*)cmd;
		response = generate_help_object(c->id, c->id_is_command);
		break;
		}
	case CMD_OUTPUT_SET: {
		struct cmd_set_output* c = (struct cmd_set_output*)cmd;
		response = self->actions.on_output_switch(self, c->target);
		break;
		}
	case CMD_CLIENT_DISCONNECT: {
		struct cmd_disconnect_client* c =
			(struct cmd_disconnect_client*)cmd;
		response = self->actions.on_disconnect_client(self, c->id);
		break;
		}
	case CMD_DETACH:
		response = self->actions.on_detach(self);
		break;
	case CMD_WAYVNC_EXIT:
		response = self->actions.on_wayvnc_exit(self);
		break;
	case CMD_VERSION:
		response = generate_version_object();
		break;
	case CMD_EVENT_RECEIVE:
		client->accept_events = true;
		response = cmd_ok();
		break;
	case CMD_CLIENT_LIST:
		response = generate_vnc_client_list(self);
		break;
	case CMD_OUTPUT_LIST:
		response = generate_output_list(self);
		break;
	case CMD_OUTPUT_CYCLE:
		response = self->actions.on_output_cycle(self, OUTPUT_CYCLE_FORWARD);
		break;
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

static int client_enqueue(struct ctl_client* self, json_t* message,
		enum send_priority priority)
{
	int result;
	switch(priority) {
	case SEND_IMMEDIATE:
		result = json_array_insert(self->response_queue, 0, message);
		break;
	case SEND_FIFO:
		result = json_array_append(self->response_queue, message);
		break;
	}
	client_set_aml_event_mask(self);
	return result;
}

static int client_enqueue_jsonipc(struct ctl_client* self,
		struct jsonipc_response* resp, enum send_priority priority)
{
	int result = 0;
	json_error_t err;
	json_t* packed_response = jsonipc_response_pack(resp, &err);
	if (!packed_response) {
		nvnc_log(NVNC_LOG_WARNING, "Pack failed: %s", err.text);
		result = -1;
		goto failure;
	}
	result = client_enqueue(self, packed_response, priority);
	json_decref(packed_response);
	if (result != 0)
		nvnc_log(NVNC_LOG_WARNING, "Append failed");
failure:
	jsonipc_response_destroy(resp);
	return result;
}

static int client_enqueue_error(struct ctl_client* self,
		struct jsonipc_error* err, json_t* id)
{
	struct jsonipc_response* resp = jsonipc_error_response_new(err, id);
	return client_enqueue_jsonipc(self, resp, SEND_FIFO);
}

static int client_enqueue__response(struct ctl_client* self,
		struct cmd_response* response, json_t* id,
		enum send_priority priority)
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
	return client_enqueue_jsonipc(self, resp, priority);
}

static int client_enqueue_response(struct ctl_client* self,
		struct cmd_response* response, json_t* id)
{
	return client_enqueue__response(self, response, id, SEND_FIFO);
}

static int client_enqueue_internal_error(struct ctl_client* self,
		struct cmd_response* err)
{
	int result = client_enqueue__response(self, err, NULL, SEND_IMMEDIATE);
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
			ctl_server_dispatch_cmd(server, client, cmd);
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

static int cleanup_old_socket(struct ctl* self, struct sockaddr* addr,
		size_t addr_size)
{
	struct stat sb;
	if (stat(self->socket_path, &sb) == -1)
		// Doesn't exist: safe to proceed.
		return 0;

	if (!S_ISSOCK(sb.st_mode)) {
		nvnc_log(NVNC_LOG_ERROR, "Socket path '%s’ exists already and is not a socket.");
		goto manual_intervention;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		FAILED_TO("open a temporary socket");
		goto manual_intervention;
	}

	nvnc_log(NVNC_LOG_DEBUG, "Connecting to existing socket in case it's stale");
	if (connect(fd, addr, addr_size) == 0) {
		close(fd);
		nvnc_log(NVNC_LOG_ERROR, "Another wayvnc process is already running.");
		nvnc_log(NVNC_LOG_ERROR, "Use the '-S' option to choose an alternate control socket location");
		return -1;
	}
	nvnc_log(NVNC_LOG_DEBUG, "Connect failed: %m");

	close(fd);
	nvnc_log(NVNC_LOG_WARNING, "Deleting stale control socket path \"%s\"", self->socket_path);
	if (unlink(self->socket_path) == -1) {
		FAILED_TO("remove stale unix socket");
		goto manual_intervention;
	}
	return 0;

manual_intervention:
	nvnc_log(NVNC_LOG_ERROR, "Manually remove \"%s\" or use the '-S' option to choose an alternate socket location", self->socket_path);
	return -1;
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

	if (cleanup_old_socket(self, (struct sockaddr*)&addr, sizeof(addr)) != 0)
		goto bind_failure;

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
	unlink(self->socket_path);
bind_failure:
	close(self->fd);
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

json_t* pack_connection_event_params(
		const struct ctl_server_client_info *info,
		int new_connection_count)
{
	// TODO: Why is the id a string?
	char id_str[64];
	snprintf(id_str, sizeof(id_str), "%d", info->id);

	return json_pack("{s:s, s:s?, s:s?, s:s?, s:i}",
			"id", id_str,
			"hostname", info->hostname,
			"username", info->username,
			"seat", info->seat,
			"connection_count", new_connection_count);
}

int ctl_server_enqueue_event(struct ctl* self, enum event_type evt_type,
		json_t* params)
{
	const char* event_name = ctl_event_list[evt_type].name;
	char* param_str = json_dumps(params, JSON_COMPACT);
	nvnc_log(NVNC_LOG_DEBUG, "Enqueueing %s event: %s", event_name, param_str);
	free(param_str);
	struct jsonipc_request* event = jsonipc_event_new(event_name, params);
	json_decref(params);
	json_error_t err;
	json_t* packed_event = jsonipc_request_pack(event, &err);
	jsonipc_request_destroy(event);
	if (!packed_event) {
		nvnc_log(NVNC_LOG_WARNING, "Could not pack %s event json: %s", event_name, err.text);
		return -1;
	}

	int enqueued = 0;
	struct ctl_client* client;
	wl_list_for_each(client, &self->clients, link) {
		if (!client->accept_events) {
			nvnc_trace("Skipping event send to control client %p", client);
			continue;
		}
		if (client_enqueue(client, packed_event, false) == 0) {
			nvnc_trace("Enqueued event for control client %p", client);
			enqueued++;
		} else {
			nvnc_trace("Failed to enqueue event for control client %p", client);
		}
	}
	json_decref(packed_event);
	nvnc_log(NVNC_LOG_DEBUG, "Enqueued %s event for %d clients", event_name, enqueued);
	return enqueued;
}

static void ctl_server_event_connect(struct ctl* self,
		enum event_type evt_type,
		const struct ctl_server_client_info *info,
		int new_connection_count)
{
	json_t* params =
		pack_connection_event_params(info, new_connection_count);
	ctl_server_enqueue_event(self, evt_type, params);
}

void ctl_server_event_connected(struct ctl* self,
		const struct ctl_server_client_info *info,
		int new_connection_count)
{
	ctl_server_event_connect(self, EVT_CLIENT_CONNECTED, info,
			new_connection_count);
}

void ctl_server_event_disconnected(struct ctl* self,
		const struct ctl_server_client_info *info,
		int new_connection_count)
{
	ctl_server_event_connect(self, EVT_CLIENT_DISCONNECTED, info,
			new_connection_count);
}

void ctl_server_event_capture_changed(struct ctl* self,
		const char* captured_output)
{
	ctl_server_enqueue_event(self, EVT_CAPTURE_CHANGED,
			json_pack("{s:s}", "output", captured_output));
}
