/*
 * Copyright (c) 2022 Andri Yngvason
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

struct wv_option {
	char short_opt;
	const char* long_opt;
	const char* schema;
	const char* help;
	const char* default_;
	const char* positional;
	bool is_subcommand;
};

struct wv_option_value {
	const struct wv_option* option;
	char value[256];
};

struct option_parser {
	const char* name;
	const struct wv_option* options;
	int n_opts;

	struct wv_option_value values[128];
	int n_values;
	int position;

	size_t remaining_argc;
	const char* const* remaining_argv;
};

void option_parser_init(struct option_parser* self,
		const struct wv_option* options);

int option_parser_print_arguments(struct option_parser* self, FILE* stream);

void option_parser_print_options(struct option_parser* self, FILE* stream);

int option_parser_parse(struct option_parser* self, int argc,
		const char* const* argv);
const char* option_parser_get_value(const struct option_parser* self,
		const char* name);
