/*
 * Copyright (c) 2022-2023 Jim Ramsay
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
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>
#include <jansson.h>
#include <sys/param.h>

#include "json-ipc.h"
#include "ctl-client.h"
#include "ctl-commands.h"
#include "ctl-server.h"
#include "strlcpy.h"
#include "util.h"
#include "option-parser.h"
#include "table-printer.h"

#define LOG(level, fmt, ...) \
	fprintf(stderr, "[%s:%d] <" level "> " fmt "\n", __FILE__, __LINE__, \
		##__VA_ARGS__)

#define WARN(fmt, ...) \
	LOG("WARNING", fmt, ##__VA_ARGS__)

static bool do_debug = false;

#define DEBUG(fmt, ...) \
	if (do_debug) \
	LOG("DEBUG", fmt, ##__VA_ARGS__)

static struct cmd_info internal_events[] = {
	{ .name = "wayvnc-startup",
		.description = "Sent by wayvncctl when a successful wayvnc control connection is established and event registration has succeeded, both upon initial startup and on subsequent registrations with --reconnect.",
		.params = {{}},
	},
	{ .name = "wayvnc-shutdown",
		.description = "Sent by wayvncctl when the wayvnc control connection is dropped, usually due to wayvnc exiting.",
		.params = {{}},
	},
};
#define EVT_LOCAL_STARTUP internal_events[0].name
#define EVT_LOCAL_SHUTDOWN internal_events[1].name
#define INTERNAL_EVT_LEN 2

struct ctl_client {
	void* userdata;
	struct sockaddr_un addr;
	unsigned flags;

	char read_buffer[1024];
	size_t read_len;

	bool wait_for_events;

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
	new->fd = -1;

	if (strlen(socket_path) >= sizeof(new->addr.sun_path)) {
		errno = ENAMETOOLONG;
		WARN("Failed to create unix socket: %m");
		goto socket_failure;
	}
	strcpy(new->addr.sun_path, socket_path);
	new->addr.sun_family = AF_UNIX;

	return new;

socket_failure:
	free(new);
	return NULL;
}

static int wait_for_socket(const char* socket_path, int timeout)
{
	bool needs_log = true;
	struct stat sb;
	while (stat(socket_path, &sb) != 0) {
		if (timeout == 0) {
			WARN("Failed to find socket path \"%s\": %m",
					socket_path);
			return 1;
		}
		if (needs_log) {
			needs_log = false;
			DEBUG("Waiting for socket path \"%s\" to appear",
					socket_path);
		}
		if (usleep(50000) == -1) {
			WARN("Failed to wait for socket path: %m");
			return -1;
		}
	}
	if (S_ISSOCK(sb.st_mode)) {
		DEBUG("Found socket \"%s\"", socket_path);
	} else {
		WARN("Path \"%s\" exists but is not a socket (0x%x)",
				socket_path, sb.st_mode);
		return -1;
	}
	return 0;
}

static int try_connect(struct ctl_client* self, int timeout)
{
	if (self->fd != -1)
		close(self->fd);
	self->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (self->fd < 0) {
		WARN("Failed to create unix socket: %m");
		return 1;
	}
	while (connect(self->fd, (struct sockaddr*)&self->addr,
				sizeof(self->addr)) != 0) {
		if (timeout == 0 || errno != ENOENT) {
			WARN("Failed to connect to unix socket \"%s\": %m",
					self->addr.sun_path);
			return 1;
		}
		if (usleep(50000) == -1) {
			WARN("Failed to wait for connect to succeed: %m");
			return 1;
		}
	}
	return 0;
}

static int ctl_client_connect(struct ctl_client* self, int timeout)
{
	// TODO: Support arbitrary timeouts?
	assert(timeout == 0 || timeout == -1);

	if (wait_for_socket(self->addr.sun_path, timeout) != 0)
		return 1;

	if (try_connect(self, timeout) != 0)
		return 1;

	return 0;
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

static struct jsonipc_request* ctl_client_parse_args(struct ctl_client* self,
		enum cmd_type* cmd, struct option_parser* options)
{
	struct jsonipc_request* request = NULL;
	json_t* params = json_object();
	struct cmd_info* info = ctl_command_by_type(*cmd);
	if (option_parser_get_value(options, "help")) {
		json_object_set_new(params, "command", json_string(info->name));
		*cmd = CMD_HELP;
		info = ctl_command_by_type(*cmd);
		goto out;
	}
	for (int i = 0; info->params[i].name != NULL; ++i) {
		const char* key = info->params[i].name;
		const char* value = option_parser_get_value(options, key);
		if (!value)
			continue;
		json_object_set_new(params, key, json_string(value));
	}
out:
	request = jsonipc_request_new(info->name, params);
	json_decref(params);
	return request;
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
		if (self->read_len == sizeof(self->read_buffer)) {
			WARN("Response message is too long");
			errno = EMSGSIZE;
		} else {
			DEBUG("Awaiting more data");
			errno = ENODATA;
		}
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
			if (errno == EINTR && self->wait_for_events)
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

static void print_error(struct jsonipc_response* response, const char* method)
{
	printf("Error (%d)", response->code);
	if (!response->data)
		goto out;
	json_t* data = response->data;
	if (json_is_string(data))
		printf(": %s", json_string_value(data));
	else if (json_is_object(data) &&
			json_is_string(json_object_get(data, "error")))
		printf(": %s", json_string_value(json_object_get(data, "error")));
	else
		json_dumpf(response->data, stdout, JSON_INDENT(2));
out:
	printf("\n");
}

static void pretty_version(json_t* data)
{
	printf("wayvnc is running:\n");
	const char* key;
	json_t* value;
	json_object_foreach(data, key, value)
		printf("  %s: %s\n", key, json_string_value(value));
}

static void pretty_client_list(json_t* data)
{
	int n = json_array_size(data);
	printf("There %s %d VNC client%s connected%s\n", (n == 1) ? "is" : "are",
			n, (n == 1) ? "" : "s", (n > 0) ? ":" : ".");
	size_t i;
	json_t* value;
	json_array_foreach(data, i, value) {
		char* id = NULL;
		char* hostname = NULL;
		char* username = NULL;
		json_unpack(value, "{s:s, s?s, s?s}", "id", &id, "hostname",
				&hostname, "username", &username);
		printf("  client[%s]: ", id);
		if (username)
			printf("%s@", username);
		printf("%s\n", hostname ? hostname : "<unknown>");
	}
}

static void pretty_output_list(json_t* data)
{
	int n = json_array_size(data);
	printf("There %s %d output%s%s\n", (n == 1) ? "is" : "are",
			n, (n == 1) ? "" : "s", (n > 0) ? ":" : ".");
	size_t i;
	json_t* value;
	json_array_foreach(data, i, value) {
		char* name = NULL;
		char* description = NULL;
		int height = -1;
		int width = -1;
		int captured = false;
		json_unpack(value, "{s:s, s:s, s:i, s:i, s:b}", "name", &name, 
				"description", &description,
				"height", &height,
				"width", &width,
				"captured", &captured);
		printf("%s output[%s]: %s (%dx%d)\n",
				captured ? "*" : " ", name, description, width,
				height);
	}
}
static void pretty_print(json_t* data,
		struct jsonipc_request* request)
{
	enum cmd_type cmd = ctl_command_parse_name(request->method);
	switch (cmd) {
	case CMD_VERSION:
		pretty_version(data);
		break;
	case CMD_CLIENT_LIST:
		pretty_client_list(data);
		break;
	case CMD_OUTPUT_LIST:
		pretty_output_list(data);
		break;
	case CMD_CLIENT_DISCONNECT:
	case CMD_OUTPUT_SET:
	case CMD_OUTPUT_CYCLE:
	case CMD_WAYVNC_EXIT:
		printf("Ok\n");
		break;
	case CMD_EVENT_RECEIVE:
	case CMD_HELP:
		abort(); // Handled directly by ctl_client_run_command
	case CMD_UNKNOWN:
		json_dumpf(data, stdout, JSON_INDENT(2));
	}
}

static void print_compact_json(json_t* data)
{
	json_dumpf(data, stdout, JSON_COMPACT);
	printf("\n");
}

static int ctl_client_print_response(struct ctl_client* self,
		struct jsonipc_request* request,
		struct jsonipc_response* response)
{
	DEBUG("Response code: %d", response->code);
	if (response->data) {
		if (self->flags & CTL_CLIENT_PRINT_JSON)
			print_compact_json(response->data);
		else if (response->code == 0)
			pretty_print(response->data, request);
		else
			print_error(response, request->method);
	}
	return response->code;
}

static struct ctl_client* sig_target = NULL;
static void stop_loop(int signal)
{
	sig_target->wait_for_events = false;
}

static void setup_signals(struct ctl_client* self)
{
	sig_target = self;
	struct sigaction sa = { 0 };
	sa.sa_handler = stop_loop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void print_indent(int level)
{
	for (int i = 0; i < level; ++i)
		printf("  ");
}

static bool json_has_content(json_t* root)
{
	if (!root)
		return false;
	size_t i;
	const char* key;
	json_t* value;
	switch (json_typeof(root)) {
	case JSON_NULL:
		return false;
	case JSON_INTEGER:
	case JSON_REAL:
	case JSON_TRUE:
	case JSON_FALSE:
		return true;
	case JSON_STRING:
		return json_string_value(root)[0] != '\0';
	case JSON_OBJECT:
		json_object_foreach(root, key, value)
			if (json_has_content(value))
				return true;
		return false;
	case JSON_ARRAY:
		json_array_foreach(root, i, value)
			if (json_has_content(value))
				return true;
		return false;
	}
	return false;
}

static void print_as_yaml(json_t* data, int level, bool needs_leading_newline)
{
	size_t i;
	const char* key;
	json_t* value;
	bool needs_indent = needs_leading_newline;
	switch(json_typeof(data)) {
	case JSON_NULL:
		printf("<null>\n");
		break;
	case JSON_OBJECT:
		if (json_object_size(data) > 0 && needs_leading_newline)
				printf("\n");
		json_object_foreach(data, key, value) {
			if (!json_has_content(value))
				continue;
			if (needs_indent)
				print_indent(level);
			else
				needs_indent = true;
			printf("%s: ", key);
			print_as_yaml(value, level + 1, true);
		}
		break;
	case JSON_ARRAY:
		if (json_array_size(data) > 0 && needs_leading_newline)
			printf("\n");
		json_array_foreach(data, i, value) {
			if (!json_has_content(value))
				continue;
			print_indent(level);
			printf("- ");
			print_as_yaml(value, level + 1, json_is_array(value));
		}
		break;
	case JSON_STRING:
		printf("%s\n", json_string_value(data));
		break;
	case JSON_INTEGER:
		printf("%" JSON_INTEGER_FORMAT "\n", json_integer_value(data));
		break;
	case JSON_REAL:
		printf("%f\n", json_real_value(data));
		break;
	case JSON_TRUE:
		printf("true\n");
		break;
	case JSON_FALSE:
		printf("false\n");
		break;
	}
}

static void print_event(struct jsonipc_request* event, unsigned flags)
{
	if (flags & CTL_CLIENT_PRINT_JSON) {
		print_compact_json(event->json);
	} else {
		printf("\n%s:", event->method);
		if (event->params)
			print_as_yaml(event->params, 1, true);
		else
			printf("<<null>\n");
	}
	fflush(stdout);
}

static void send_local_event(struct ctl_client* self, const char* name)
{
	struct jsonipc_request* event = jsonipc_event_new(name, NULL);
	event->json = jsonipc_request_pack(event, NULL);
	print_event(event, self->flags);
	jsonipc_request_destroy(event);
}

static void send_startup_event(struct ctl_client* self)
{
	send_local_event(self, EVT_LOCAL_STARTUP);
}

static void send_shutdown_event(struct ctl_client* self)
{
	send_local_event(self, EVT_LOCAL_SHUTDOWN);
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

static struct jsonipc_response* ctl_client_run_single_command(struct ctl_client* self,
		struct jsonipc_request* request)
{
	if (ctl_client_send_request(self, request) < 0)
		return NULL;

	return ctl_client_wait_for_response(self);
}

static int ctl_client_register_for_events(struct ctl_client* self,
		struct jsonipc_request* request)
{
	struct jsonipc_response* response = ctl_client_run_single_command(self, request);
	if (!response)
		return -1;

	int result = response->code;
	jsonipc_response_destroy(response);
	if (result == 0)
		send_startup_event(self);
	return result;
}

static int ctl_client_reconnect_event_loop(struct ctl_client* self,
		struct jsonipc_request* request, int timeout)
{
	if (ctl_client_connect(self, timeout) != 0)
		return -1;
	return ctl_client_register_for_events(self, request);
}

static int ctl_client_event_loop(struct ctl_client* self,
		struct jsonipc_request* request)
{
	int result = ctl_client_register_for_events(self, request);
	if (result != 0)
		return result;

	self->wait_for_events = true;
	setup_signals(self);
	while (self->wait_for_events) {
		DEBUG("Waiting for an event");
		json_t* root = read_one_object(self, -1);
		if (!root) {
			if (errno == ECONNRESET) {
				send_shutdown_event(self);
				if (self->flags & CTL_CLIENT_RECONNECT &&
						ctl_client_reconnect_event_loop(
							self, request, -1) == 0)
					continue;
			}
			break;
		}
		struct jsonipc_error err = JSONIPC_ERR_INIT;
		struct jsonipc_request* event = jsonipc_event_parse_new(root, &err);
		json_decref(root);
		print_event(event, self->flags);
		jsonipc_request_destroy(event);
	}
	return 0;
}

static int ctl_client_print_single_command(struct ctl_client* self,
		struct jsonipc_request* request)
{
	struct jsonipc_response* response = ctl_client_run_single_command(self,
			request);
	if (!response)
		return 1;
	int result = ctl_client_print_response(self, request, response);
	jsonipc_response_destroy(response);
	return result;
}

void ctl_client_print_command_list(FILE* stream)
{
	fprintf(stream, "Commands:\n");
	size_t max_namelen = 0;
	for (size_t i = 0; i < CMD_LIST_LEN; ++i) {
		if (i == CMD_HELP) // hidden
			continue;
		max_namelen = MAX(max_namelen, strlen(ctl_command_list[i].name));
	}
	struct table_printer printer;
	table_printer_init(&printer, stdout, max_namelen);
	for (size_t i = 0; i < CMD_LIST_LEN; ++i) {
		if (i == CMD_HELP) // hidden
			continue;
		table_printer_print_line(&printer, ctl_command_list[i].name,
				ctl_command_list[i].description);
	}
	fprintf(stream, "\nRun 'wayvncctl command-name --help' for command-specific details.\n");
}

static void print_event_info(const struct cmd_info* info)
{
	printf("%s\n\n", info->name);
	table_printer_indent_and_reflow_text(stdout, info->description, 80, 0, 0);
	if (info->params[0].name != NULL) {
		printf("\nData fields:\n");
		size_t max_namelen = 0;
		for (int i = 0; info->params[i].name != NULL; ++i)
			max_namelen = MAX(max_namelen, strlen(info->params[i].name));

		struct table_printer printer;
		table_printer_init(&printer, stdout, max_namelen);
		for (int i = 0; info->params[i].name != NULL; ++i)
			table_printer_print_fmtline(&printer,
					info->params[i].description,
					"%s=...", info->params[i].name);
	}
}

static int print_event_details(const char* evt_name)
{
	struct cmd_info* info = ctl_event_by_name(evt_name);
	if (info) {
		print_event_info(info);
		return 0;
	}
	for (size_t i = 0; i < INTERNAL_EVT_LEN; ++i) {
		if (strcmp(evt_name, internal_events[i].name) == 0) {
			print_event_info(&internal_events[i]);
			return 0;
		}
	}
	WARN("No such event \"%s\"\n", evt_name);
	return 1;
}

void ctl_client_print_event_list(FILE* stream)
{
	printf("Events:\n");
	size_t max_namelen = 0;
	for (size_t i = 0; i < EVT_LIST_LEN; ++i)
		max_namelen = MAX(max_namelen, strlen(ctl_event_list[i].name));

	for (size_t i = 0; i < INTERNAL_EVT_LEN; ++i)
		max_namelen = MAX(max_namelen, strlen(internal_events[i].name));

	struct table_printer printer;
	table_printer_init(&printer, stdout, max_namelen);
	for (size_t i = 0; i < EVT_LIST_LEN; ++i)
		table_printer_print_line(&printer, ctl_event_list[i].name,
				ctl_event_list[i].description);
	for (size_t i = 0; i < INTERNAL_EVT_LEN; ++i)
		table_printer_print_line(&printer, internal_events[i].name,
				internal_events[i].description);
}

static int print_command_usage(struct ctl_client* self,
		enum cmd_type cmd,
		struct option_parser* cmd_options,
		struct option_parser* parent_options)
{
	if (self->flags & CTL_CLIENT_PRINT_JSON) {
		WARN("JSON output is not supported for \"help\" output");
		return 1;
	}
	struct cmd_info* info = ctl_command_by_type(cmd);
	if (!info) {
		WARN("No such command");
		return 1;
	}
	printf("Usage: wayvncctl [options] %s ", info->name);
	for (int i = 0; i < cmd_options->n_opts; ++i)
		if (cmd_options->options[i].positional)
			printf("<%s> ", cmd_options->options[i].positional);

	printf("[parameters]\n\n");
	table_printer_indent_and_reflow_text(stdout, info->description, 80, 0, 0);
	printf("\n");
	if (option_parser_print_arguments(cmd_options, stdout))
		printf("\n");
	option_parser_print_options(cmd_options, stdout);
	printf("\n");
	option_parser_print_options(parent_options, stdout);
	if (cmd == CMD_EVENT_RECEIVE) {
		printf("\n");
		ctl_client_print_event_list(stdout);
	}
	return 0;
}

int ctl_client_init_cmd_parser(struct option_parser* parser, enum cmd_type cmd)
{
	struct cmd_info* info = ctl_command_by_type(cmd);
	if (!info) {
		printf("Invalid command");
		return -1;
	}

	size_t param_count = 0;
	while (info->params[param_count].name != NULL)
		param_count++;

	// Add 2: one for --help and one to null-terminate the list
	size_t alloc_count = param_count + 2;
	if (cmd == CMD_EVENT_RECEIVE)
		alloc_count++;
	struct wv_option* options = calloc(alloc_count,
			sizeof(struct wv_option));
	size_t i = 0;
	if (param_count == 1) {
		// Represent a single parameter as a positional argument
		options[0].positional = info->params[0].name;
		options[0].help = info->params[0].description;
		i++;
	} else {
		for (; i < param_count; ++i) {
			struct wv_option* option = &options[i];
			option->long_opt = info->params[i].name;
			option->help = info->params[i].description;
			option->schema = "<value>";
		}
	}
	if (cmd == CMD_EVENT_RECEIVE) {
		options[i].long_opt = "show";
		options[i].schema = "<event-name>";
		options[i].help = "Display details about the given event";
		i++;
	}
	options[i].long_opt = "help";
	options[i].short_opt = 'h';
	options[i].help = "Display this help text";
	option_parser_init(parser, options);
	parser->name = "Parameters";
	return 0;
}

static void ctl_client_destroy_cmd_parser(struct option_parser* parser)
{
	// const in the struct, but we allocated it above
	free((void*)parser->options);
}

int ctl_client_run_command(struct ctl_client* self,
		struct option_parser* parent_options, unsigned flags)
{
	self->flags = flags;
	int result = 1;

	const char* method = option_parser_get_value(parent_options, "command");
	enum cmd_type cmd = ctl_command_parse_name(method);
	if (cmd == CMD_UNKNOWN || cmd == CMD_HELP) {
		WARN("No such command \"%s\"\n", method);
		return 1;
	}

	struct option_parser cmd_options = { };
	if (ctl_client_init_cmd_parser(&cmd_options, cmd) != 0)
		return 1;

	if (option_parser_parse(&cmd_options, parent_options->remaining_argc,
				parent_options->remaining_argv) != 0)
		goto parse_failure;

	if (option_parser_get_value(&cmd_options, "help")) {
		result = print_command_usage(self, cmd,
				&cmd_options, parent_options);
		goto help_printed;
	}
	if (cmd == CMD_EVENT_RECEIVE && option_parser_get_value(&cmd_options, "show")) {
		result = print_event_details(option_parser_get_value(&cmd_options, "show"));
		goto help_printed;
	}

	struct jsonipc_request*	request = ctl_client_parse_args(self, &cmd,
			&cmd_options);
	if (!request)
		goto parse_failure;

	int timeout = (flags & CTL_CLIENT_SOCKET_WAIT) ? -1 : 0;
	result = ctl_client_connect(self, timeout);
	if (result != 0)
		goto connect_failure;

	switch (cmd) {
	case CMD_EVENT_RECEIVE:
		result = ctl_client_event_loop(self, request);
		break;
	default:
		result = ctl_client_print_single_command(self, request);
		break;
	}

connect_failure:
	jsonipc_request_destroy(request);
help_printed:
parse_failure:
	ctl_client_destroy_cmd_parser(&cmd_options);
	return result;
}
