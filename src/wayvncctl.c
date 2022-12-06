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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
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
	static const char* usage =
"Usage: wayvncctl [options] [command [--param1=value1 ...]]\n"
"\n"
"Connects to and interacts with a running wayvnc instance.";
	fprintf(stream, "%s\n\n", usage);
	option_parser_print_options(options, stream);
	fprintf(stream, "\n");
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
		{ '\0', NULL, NULL, NULL }
	};

	struct option_parser option_parser;
	option_parser_init(&option_parser, opts,
			OPTION_PARSER_STOP_ON_FIRST_NONOPTION);

	while (1) {
		int c = option_parser_getopt(&option_parser, argc, argv);
		if (c < 0)
			break;

		switch (c) {
		case 'S':
			socket_path = optarg;
			break;
		case 'w':
			flags |= CTL_CLIENT_SOCKET_WAIT;
			break;
		case 'r':
			flags |= CTL_CLIENT_RECONNECT;
			break;
		case 'j':
			flags |= CTL_CLIENT_PRINT_JSON;
			break;
		case 'v':
			verbose = true;
			break;
		case 'V':
			return show_version();
		case 'h':
			return wayvncctl_usage(stdout, &option_parser, 0);
		default:
			return wayvncctl_usage(stderr, &option_parser, 1);
		}
	}
	argc -= optind;
	if (argc <= 0)
		return wayvncctl_usage(stderr, &option_parser, 1);
	argv = &argv[optind];

	ctl_client_debug_log(verbose);

	self.ctl = ctl_client_new(socket_path, &self);
	if (!self.ctl)
		goto ctl_client_failure;

	int result = ctl_client_run_command(self.ctl, argc, argv, flags);

	ctl_client_destroy(self.ctl);

	return result;

ctl_client_failure:
	return 1;
}
