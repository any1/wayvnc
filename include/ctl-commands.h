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

enum cmd_type {
	CMD_HELP,
	CMD_VERSION,
	CMD_EVENT_RECEIVE,
	CMD_SET_OUTPUT,
	CMD_GET_CLIENTS,
	CMD_GET_OUTPUTS,
	CMD_DISCONNECT_CLIENT,
	CMD_WAYVNC_EXIT,
	CMD_UNKNOWN,
};
#define CMD_LIST_LEN CMD_UNKNOWN

enum event_type {
	EVT_CLIENT_CONNECTED,
	EVT_CLIENT_DISCONNECTED,
	EVT_CAPTURE_CHANGED,
	EVT_UNKNOWN,
};
#define EVT_LIST_LEN EVT_UNKNOWN

struct cmd_param_info {
	char* name;
	char* description;
};

struct cmd_info {
	char* name;
	char* description;
	struct cmd_param_info params[5];
};

enum cmd_type ctl_command_parse_name(const char* name);
struct cmd_info* ctl_command_by_type(enum cmd_type type);
struct cmd_info* ctl_command_by_name(const char* name);

enum event_type ctl_event_parse_name(const char* name);
struct cmd_info* ctl_event_by_type(enum event_type type);
struct cmd_info* ctl_event_by_name(const char* name);

extern struct cmd_info ctl_command_list[];
extern struct cmd_info ctl_event_list[];
