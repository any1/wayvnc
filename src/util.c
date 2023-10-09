/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"

const char* wayvnc_version =
#if defined(PROJECT_VERSION)
		PROJECT_VERSION;
#else
		"UNKNOWN";
#endif

const char* default_ctl_socket_path()
{
	static char buffer[128];
	char* xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (xdg_runtime)
		snprintf(buffer, sizeof(buffer),
				"%s/wayvncctl", xdg_runtime);
	else
		snprintf(buffer, sizeof(buffer),
				"/tmp/wayvncctl-%d", getuid());
	return buffer;
}

void advance_read_buffer(char (*buffer)[], size_t* current_len, size_t advance_by)
{
	ssize_t remainder = *current_len - advance_by;
	if (remainder < 0)
		remainder = 0;
	else if (remainder > 0)
		memmove(*buffer, *buffer + advance_by, remainder);
	*current_len = remainder;
}
