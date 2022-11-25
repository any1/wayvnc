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

#include "output.h"

struct ctl;
struct cmd_response;

struct ctl_server_vnc_client {
	char id[64];
	char hostname[256];
	char username[256];
};

struct ctl_server_output {
	char name[65];
	char description[128];
	unsigned height;
	unsigned width;
	bool captured;
};

struct ctl_server_actions {
	void* userdata;
	struct cmd_response* (*on_output_cycle)(struct ctl*,
			enum output_cycle_direction direction);
	struct cmd_response* (*on_output_switch)(struct ctl*,
			const char* output_name);
	struct cmd_response* (*on_disconnect_client)(struct ctl*,
			const char* id);

	// Return number of elements created
	// Allocate 'clients' array or set ton ULL if none
	// Receiver will free(clients) when done.
	int (*get_client_list)(struct ctl*,
			struct ctl_server_vnc_client** clients);

	// Return number of elements created
	// Allocate 'outputs' array or set to NULL if none
	// Receiver will free(outputs) when done.
	int (*get_output_list)(struct ctl*,
			struct ctl_server_output** outputs);
};

struct ctl* ctl_server_new(const char* socket_path,
		const struct ctl_server_actions* actions);
void ctl_server_destroy(struct ctl*);
void* ctl_server_userdata(struct ctl*);

struct cmd_response* cmd_ok(void);
struct cmd_response* cmd_failed(const char* fmt, ...);

void ctl_server_event_connected(struct ctl*,
		const char* client_id,
		const char* client_hostname,
		const char* client_username,
		int new_connection_count);

void ctl_server_event_disconnected(struct ctl*,
		const char* client_id,
		const char* client_hostname,
		const char* client_username,
		int new_connection_count);

void ctl_server_event_capture_changed(struct ctl*,
		const char* captured_output);
