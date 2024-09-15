/*
 * Copyright (c) 2020 Andri Yngvason
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
#include <ctype.h>
#include <libgen.h>
#include <limits.h>

#include "cfg.h"

#define XSTR(s) STR(s)
#define STR(s) #s

static char* cfg__get_default_path(void)
{
	static char result[256];

	char* xdg_config_home = getenv("XDG_CONFIG_HOME");
	if (xdg_config_home) {
		snprintf(result, sizeof(result), "%s/wayvnc/config",
		         xdg_config_home);
		result[sizeof(result) - 1] = '\0';
		return result;
	}

	char* home = getenv("HOME");
	if (!home)
		return NULL;

	snprintf(result, sizeof(result), "%s/.config/wayvnc/config", home);
	result[sizeof(result) - 1] = '\0';
	return result;
}

static char* cfg__trim_left(char* str)
{
	while (isspace(*str))
		++str;
	return str;
}

static char* cfg__trim_right(char* str)
{
	char* end = str + strlen(str) - 1;
	while (str < end && isspace(*end))
		*end-- = '\0';
	return str;
}

static inline char* cfg__trim(char* str)
{
	return cfg__trim_right(cfg__trim_left(str));
}

static int cfg__load_key_value(struct cfg* self,
                               const char* key, const char* value)
{
#define LOAD_bool(v) (strcmp(v, "false") != 0)
#define LOAD_string(v) strdup(v)
#define LOAD_uint(v) strtoul(v, NULL, 0)

#define X(type, name) \
	if (strcmp(XSTR(name), key) == 0) { \
		self->name = LOAD_ ## type(value); \
		return 0; \
	}

	X_CFG_LIST
#undef X

#undef LOAD_uint
#undef LOAD_string
#undef LOAD_bool
	return -1;
}

static int cfg__load_line(struct cfg* self, char* line)
{
	line = cfg__trim(line);

	if (line[0] == '\0' || line[0] == '#')
		return 0;

	char* delim = strchr(line, '=');
	if (!delim)
		return -1;

	*delim = '\0';

	char* key = cfg__trim_right(line);
	char* value = cfg__trim_left(delim + 1);

	return cfg__load_key_value(self, key, value);
}

static char* cfg__dirname(const char* path)
{
	char buffer[PATH_MAX];
	return strdup(dirname(realpath(path, buffer)));
}

int cfg_load(struct cfg* self, const char* requested_path)
{
	const char* path = requested_path ? requested_path
	                                  : cfg__get_default_path();
	if (!path)
		return -1;

	FILE* stream = fopen(path, "r");
	if (!stream)
		return -1;

	self->directory = cfg__dirname(path);

	char* line = NULL;
	size_t len = 0;
	int lineno = 0;

	while (getline(&line, &len, stream) > 0) {
		++lineno;

		if (cfg__load_line(self, line) < 0)
			goto failure;
	}

	free(line);
	fclose(stream);
	return 0;

failure:
	cfg_destroy(self);
	free(line);
	fclose(stream);
	return lineno;
}

void cfg_destroy(struct cfg* self)
{
#define DESTROY_bool(...)
#define DESTROY_uint(...)
#define DESTROY_string(p) free(p)

#define X(type, name) DESTROY_ ## type(self->name);
	X_CFG_LIST
#undef X

#undef DESTROY_string
#undef DESTROY_uint
#undef DESTROY_bool
	free(self->directory);
}
