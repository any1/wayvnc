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

#include "ctl-commands.h"

#include <stdlib.h>
#include <string.h>

struct cmd_info ctl_command_list[] = {
	[CMD_ATTACH] = { "attach",
		"Attach to a running wayland compositor",
		{
			{ "display", "Display name", "<name>",
				.positional = true },
			{},
		}

	},
	[CMD_DETACH] = { "detach",
		"Detach from the wayland compositor",
		{{}},
	},
	[CMD_HELP] = { "help",
		"List all commands and events, or show usage of a specific command or event",
		{
			{ "command",
				"The command to show (optional)",
				"<name>" },
			{ "event",
				"The event to show (optional)",
				"<name>" },
			{},
		}
	},
	[CMD_VERSION] = { "version",
		"Query the version of the wayvnc process",
		{{}}
	},
	[CMD_EVENT_RECEIVE] = { "event-receive",
		"Register to begin receiving asynchronous events from wayvnc",
		// TODO: Event type filtering?
		{{}}
	},
	[CMD_CLIENT_LIST] = { "client-list",
		"Return a list of all currently connected VNC sessions",
		{{}}
	},
	[CMD_CLIENT_DISCONNECT] = { "client-disconnect",
		"Disconnect a VNC session",
		{
			{ "id",
				"The ID of the client to disconnect",
				"<integer>", true },
			{},
		}
	},
	[CMD_OUTPUT_LIST] = { "output-list",
		"Return a list of all currently detected Wayland outputs",
		{{}}
	},
	[CMD_OUTPUT_CYCLE] = { "output-cycle",
		"Cycle the actively captured output to the next available output, wrapping through all outputs.",
		{{}}
	},
	[CMD_OUTPUT_SET] = { "output-set",
		"Switch the actively captured output",
		{
			{ "output-name",
				"The specific output name to capture",
				"<string>", true },
			{},
		}
	},
	[CMD_WAYVNC_EXIT] = { "wayvnc-exit",
		"Disconnect all clients and shut down wayvnc",
		{{}},
	},
};

#define CLIENT_EVENT_PARAMS(including) \
	{ "id", \
		"A unique identifier for this client", \
		"<integer>" }, \
	{ "connection_count", \
		"The total number of connected VNC clients " including " this one.", \
		"<integer>" }, \
	{ "hostname", \
		"The hostname or IP address of this client (may be null)", \
		"<name|ip>" }, \
	{ "username", \
		"The username used to authentice this client (may be null).", \
		"<string>" }, \
	{},

struct cmd_info ctl_event_list[] = {
	[EVT_CAPTURE_CHANGED] = {"capture-changed",
		"Sent by wayvnc when the captured output is changed",
		{
			{ "output-name",
				"The name of the output now being captured",
				"<string>" },
			{},
		},
	},
	[EVT_CLIENT_CONNECTED] = {"client-connected",
		"Sent by wayvnc when a new VNC client connects",
		{ CLIENT_EVENT_PARAMS("including") }
	},
	[EVT_CLIENT_DISCONNECTED] = {"client-disconnected",
		"Sent by waynvc when a VNC client disconnects",
		{ CLIENT_EVENT_PARAMS("not including") }
	},
	[EVT_DETACHED] = {"detached",
		"Sent after detaching from compositor",
		{}
	},
};

enum cmd_type ctl_command_parse_name(const char* name)
{
	if (!name || name[0] == '\0')
		return CMD_UNKNOWN;
	for (size_t i = 0; i < CMD_LIST_LEN; ++i) {
		if (strcmp(name, ctl_command_list[i].name) == 0) {
			return i;
		}
	}
	return CMD_UNKNOWN;
}

enum event_type ctl_event_parse_name(const char* name)
{
	if (!name || name[0] == '\0')
		return EVT_UNKNOWN;
	for (size_t i = 0; i < EVT_LIST_LEN; ++i) {
		if (strcmp(name, ctl_event_list[i].name) == 0) {
			return i;
		}
	}
	return EVT_UNKNOWN;
}

struct cmd_info* ctl_command_by_type(enum cmd_type cmd)
{
	if (cmd == CMD_UNKNOWN)
		return NULL;
	return &ctl_command_list[cmd];
}

struct cmd_info* ctl_command_by_name(const char* name)
{
	return ctl_command_by_type(ctl_command_parse_name(name));
}

struct cmd_info* ctl_event_by_type(enum event_type evt)
{
	if (evt == EVT_UNKNOWN)
		return NULL;
	return &ctl_event_list[evt];
}

struct cmd_info* ctl_event_by_name(const char* name)
{
	return ctl_event_by_type(ctl_event_parse_name(name));
}
