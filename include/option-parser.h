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
#include <getopt.h>

struct wv_option {
	char short_opt;
	const char* long_opt;
	const char* schema;
	const char* help;
};

struct option_parser {
	const struct wv_option* options;
	char short_opts[128];
	struct option long_opts[128];
	int n_opts;
};

void option_parser_init(struct option_parser* self, const struct wv_option* options);

void option_parser_print_usage(struct option_parser* self, FILE* stream);
int option_parser_getopt(struct option_parser* self, int argc, char* argv[]);
