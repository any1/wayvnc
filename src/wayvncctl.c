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

#define MAYBE_UNUSED __attribute__((unused))

struct wayvncctl {
	bool do_exit;

	struct ctl_client* ctl;
};

static int wayvncctl_usage(FILE* stream, int rc)
{
	static const char* usage =
"Usage: wayvncctl [options] [command [--param1=value1 ...]]\n"
"\n"
"Connects to and interacts with a running wayvnc instance."
"\n"
"Try 'wayvncctl help' for a list of available commands.\n"
"\n"
"Options:\n"
"    -S,--socket=<path>                   Wayvnc control socket path.\n"
"                                         Default: $XDG_RUNTIME_DIR/wayvncctl\n"
"    -w,--wait                            Wait for wayvnc to start up if it's\n"
"                                         not already running.\n"
"    -j,--json                            Output json on stdout.\n"
"    -V,--version                         Show version info.\n"
"    -v,--verbose                         Be more verbose.\n"
"    -h,--help                            Get help (this text).\n"
"\n";

	fprintf(stream, "%s", usage);

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

	static const char* shortopts = "+S:hVvjw";

	bool verbose = false;
	const char* socket_path = NULL;
	int wait_for_socket = 0;

	unsigned flags = 0;

	static const struct option longopts[] = {
		{ "socket", required_argument, NULL, 'S' },
		{ "wait", no_argument, NULL, 'w' },
		{ "json", no_argument, NULL, 'j' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'S':
			socket_path = optarg;
			break;
		case 'w':
			wait_for_socket = -1;
			break;
		case 'j':
			flags |= PRINT_JSON;
			break;
		case 'v':
			verbose = true;
			break;
		case 'V':
			return show_version();
		case 'h':
			return wayvncctl_usage(stdout, 0);
		default:
			return wayvncctl_usage(stderr, 1);
		}
	}
	argc -= optind;
	argv = &argv[optind];

	ctl_client_debug_log(verbose);

	self.ctl = ctl_client_new(socket_path, &self);
	if (!self.ctl)
		goto ctl_client_failure;

	int result = ctl_client_connect(self.ctl, wait_for_socket);
	if (result != 0)
		goto socket_failure;

	result = ctl_client_run_command(self.ctl, argc, argv, flags);

socket_failure:
	ctl_client_destroy(self.ctl);

	return result;

ctl_client_failure:
	return 1;
}
