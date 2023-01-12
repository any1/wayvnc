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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util.h"
#include "ctl-client.h"
#include "option-parser.h"

#define MAYBE_UNUSED __attribute__((unused))

struct wayvncctl {
	bool do_exit;

	struct ctl_client* ctl;
};

static int wayvncctl_usage(FILE* stream, struct option_parser* options, int rc)
{
	fputs("Usage: wayvncctl", stream);
	option_parser_print_usage(options, stream);
	fputs(" [parameters]\n\n", stream);
	fputs("Connects to and interacts with a running wayvnc instance.\n\n", stream);
	option_parser_print_options(options, stream);
	fputc('\n', stream);
	ctl_client_print_command_list(stream);
	return rc;
}

static int show_version(void)
{
	printf("wayvnc: %s\n", wayvnc_version);
	return 0;
}

int main(int argc, char* argv[])
{
	struct wayvncctl self = { 0 };

	bool verbose = false;
	const char* socket_path = NULL;

	unsigned flags = 0;

	static const struct wv_option opts[] = {
		{ .positional = "command",
		  .is_subcommand = true },
		{ 'S', "socket", "<path>",
		  "Control socket path." },
		{ 'w', "wait", NULL,
                  "Wait for wayvnc to start up if it's not already running." },
		{ 'r', "reconnect", NULL,
		  "If disconnected while waiting for events, wait for wayvnc to restart." },
		{ 'j', "json", NULL,
		  "Output json on stdout." },
		{ 'V', "version", NULL,
		  "Show version info." },
		{ 'v', "verbose", NULL,
		  "Be more verbose." },
		{ 'h', "help", NULL,
		  "Get help (this text)." },
		{ }
	};

	struct option_parser option_parser;
	option_parser_init(&option_parser, opts);
	if (option_parser_parse(&option_parser, argc,
				(const char* const*)argv) < 0)
		return wayvncctl_usage(stderr, &option_parser, 1);

	if (option_parser_get_value(&option_parser, "help"))
		return wayvncctl_usage(stdout, &option_parser, 0);

	if (option_parser_get_value(&option_parser, "version"))
		return show_version();

	socket_path = option_parser_get_value(&option_parser, "socket");
	flags |= option_parser_get_value(&option_parser, "wait")
		? CTL_CLIENT_SOCKET_WAIT : 0;
	flags |= option_parser_get_value(&option_parser, "reconnect")
		? CTL_CLIENT_RECONNECT : 0;
	flags |= option_parser_get_value(&option_parser, "json")
		? CTL_CLIENT_PRINT_JSON : 0;
	verbose = !!option_parser_get_value(&option_parser, "verbose");

	// No command; nothing to do...
	if (!option_parser_get_value(&option_parser, "command"))
		return wayvncctl_usage(stdout, &option_parser, 1);

	ctl_client_debug_log(verbose);

	self.ctl = ctl_client_new(socket_path, &self);
	if (!self.ctl)
		goto ctl_client_failure;

	int result = ctl_client_run_command(self.ctl, &option_parser, flags);

	ctl_client_destroy(self.ctl);

	return result;

ctl_client_failure:
	return 1;
}
