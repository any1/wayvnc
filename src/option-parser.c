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

#include "option-parser.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

static int count_options(const struct wv_option* opts)
{
	int n = 0;
	while (opts[n].short_opt || opts[n].long_opt)
		n++;
	return n;
}

void option_parser_init(struct option_parser* self,
		const struct wv_option* options,
		unsigned flags)
{
	memset(self, 0, sizeof(*self));

	self->options = options;
	self->n_opts = count_options(options);

	int short_opt_index = 0;
	int long_opt_index = 0;

	if (flags && OPTION_PARSER_STOP_ON_FIRST_NONOPTION)
		self->short_opts[short_opt_index++] = '+';

	for (int i = 0; i < self->n_opts; ++i) {
		assert(options[i].short_opt); // TODO: Make this optional?

		self->short_opts[short_opt_index++] = options[i].short_opt;
		if (options[i].schema)
			self->short_opts[short_opt_index++] = ':';

		if (!options[i].long_opt)
			continue;

		const struct wv_option* src_opt = &options[i];
		struct option* dst_opt = &self->long_opts[long_opt_index++];

		dst_opt->val = src_opt->short_opt;
		dst_opt->name = src_opt->long_opt;
		dst_opt->has_arg = src_opt->schema ?
			required_argument : no_argument;
	}
}

static int get_left_col_width(const struct wv_option* opts, int n)
{
	int max_width = 0;

	for (int i = 0; i < n; ++i) {
		int width = 0;

		if (opts[i].short_opt)
			width += 2;

		if (opts[i].long_opt)
			width += 2 + strlen(opts[i].long_opt);

		if (opts[i].short_opt && opts[i].long_opt)
			width += 1; // for ','

		if (opts[i].schema) {
			width += strlen(opts[i].schema);

			if (opts[i].long_opt)
				width += 1; // for '='
		}

		if (width > max_width)
			max_width = width;
	}

	return max_width;
}

static void reflow_text(char* dst, const char* src, int width)
{
	int line_len = 0;
	int last_space_pos = 0;
	
	int dst_len = 0;
	int i = 0; 

	while (src[i]) {
		char c = src[i];

		if (line_len > width) {
			assert(last_space_pos > 0);

			dst_len -= i - last_space_pos;
			dst[dst_len++] = '\n';
			i = last_space_pos + 1;
			line_len = 0;
			continue;
		}

		if (c == ' ')
			last_space_pos = i;

		dst[dst_len++] = c;
		++i;
		++line_len;
	}

	dst[dst_len] = '\0';
}

static void format_option(const struct wv_option* opt, int left_col_width,
		FILE* stream)
{
	assert(opt->help);

	int n_chars = fprintf(stream, "    ");
	if (opt->short_opt)
		n_chars += fprintf(stream, "-%c", opt->short_opt);
	if (opt->long_opt)
		n_chars += fprintf(stream, "%s--%s",
				opt->short_opt ? "," : "", opt->long_opt);
	if (opt->schema)
		n_chars += fprintf(stream, "%s%s",
				opt->long_opt ? "=" : "", opt->schema);

	n_chars += fprintf(stream, "%*s", left_col_width - n_chars + 8, "");

	int right_col_width = 80 - 8 - left_col_width;
	assert(right_col_width >= 0);

	char help[256];
	reflow_text(help, opt->help, right_col_width);

	char* line = strtok(help, "\n");
	fprintf(stream, "%s\n", line);

	while (true) {
		line = strtok(NULL, "\n");
		if (!line)
			break;

		fprintf(stream, "%*s%s\n", left_col_width + 8, "", line);
	}
}

void option_parser_print_options(struct option_parser* self, FILE* stream)
{
	fprintf(stream, "Options:\n");
	int left_col_width = get_left_col_width(self->options, self->n_opts);

	for (int i = 0; i < self->n_opts; ++i) {
		format_option(&self->options[i], left_col_width, stream);
	}
}

int option_parser_getopt(struct option_parser* self, int argc, char* argv[])
{
	return getopt_long(argc, argv, self->short_opts, self->long_opts, NULL);
}
