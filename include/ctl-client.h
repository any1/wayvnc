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

#include <stdio.h>
#include <stdbool.h>

struct ctl_client;
struct option_parser;

void ctl_client_debug_log(bool enable);

struct ctl_client* ctl_client_new(const char* socket_path, void* userdata);
void ctl_client_destroy(struct ctl_client*);
void* ctl_client_userdata(struct ctl_client*);

#define CTL_CLIENT_PRINT_JSON  (1 << 0)
#define CTL_CLIENT_SOCKET_WAIT (1 << 1)
#define CTL_CLIENT_RECONNECT   (1 << 2)

int ctl_client_run_command(struct ctl_client* self,
		struct option_parser* parent_options, unsigned flags);

void ctl_client_print_command_list(FILE* stream);
void ctl_client_print_event_list(FILE* stream);
