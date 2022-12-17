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
#include "strlcpy.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int count_options(const struct wv_option* opts)
{
	int n = 0;
	while (opts[n].short_opt || opts[n].long_opt || opts[n].positional)
		n++;
	return n;
}

void option_parser_init(struct option_parser* self,
		const struct wv_option* options)
{
	memset(self, 0, sizeof(*self));

	self->options = options;
	self->n_opts = count_options(options);
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
	if (!opt->help)
		return;

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

static const struct wv_option* find_long_option(
		const struct option_parser* self, const char* name)
{
	for (int i = 0; i < self->n_opts; ++i) {
		if (!self->options[i].long_opt)
			continue;

		if (strcmp(self->options[i].long_opt, name) == 0)
			return &self->options[i];
	}
	return NULL;
}

static const struct wv_option* find_short_option(
		const struct option_parser* self, char name)
{
	for (int i = 0; i < self->n_opts; ++i)
		if (self->options[i].short_opt == name)
			return &self->options[i];
	return NULL;
}

static const struct wv_option* find_positional_option(
		struct option_parser* self, int position)
{
	int current_pos = 0;
	for (int i = 0; i < self->n_opts; ++i) {
		if (!self->options[i].positional)
			continue;

		if (current_pos == position)
			return &self->options[i];

		current_pos += 1;
	}
	return NULL;
}

static int append_value(struct option_parser* self,
		const struct wv_option* option, const char* value)
{
	if ((size_t)self->n_values >= ARRAY_SIZE(self->values)) {
		fprintf(stderr, "ERROR: Too many arguments!\n");
		return -1;
	}

	struct wv_option_value* dst = &self->values[self->n_values++];
	dst->option = option;
	strlcpy(dst->value, value, sizeof(dst->value));

	return 0;
}

static int parse_long_arg(struct option_parser* self, int argc,
		const char* const* argv, int index)
{
	int count = 1;
	char name[256];
	strlcpy(name, argv[index] + 2, sizeof(name));
	char* eq = strchr(name, '=');
	if (eq)
		*eq = '\0';

	const struct wv_option* opt = find_long_option(self, name);
	if (!opt) {
		fprintf(stderr, "ERROR: Unknown option: \"%s\"\n", name);
		return -1;
	}

	const char* value = "1";
	if (opt->schema) {
		if (eq) {
			value = eq + 1;
		} else {
			if (index + 1 >= argc) {
				fprintf(stderr, "ERROR: An argument is required for the \"%s\" option\n",
						opt->long_opt);
				return -1;
			}

			value = argv[index + 1];
			count += 1;
		}
	}

	if (append_value(self, opt, value) < 0)
		return -1;

	return count;
}

static int parse_short_args(struct option_parser* self, char argc,
		const char* const* argv, int index)
{
	int count = 1;
	int len = strlen(argv[index]);

	for (int i = 1; i < len; ++i) {
		char name = argv[index][i];
		const struct wv_option* opt = find_short_option(self, name);
		if (!opt) {
			fprintf(stderr, "ERROR: Unknown option: \"%c\"\n", name);
			return -1;
		}

		const char* value = "1";
		if (opt->schema) {
			const char* tail = argv[index] + i + 1;
			if (tail[0] == '=') {
				value = tail + 1;
			} else if (tail[0]) {
				value = tail;
			} else {
				if (index + 1 >= argc) {
					fprintf(stderr, "ERROR: An argument is required for the \"%c\" option\n",
							opt->short_opt);

					return -1;
				}

				value = argv[index + 1];
				count += 1;
			}
		}

		if (append_value(self, opt, value) < 0)
			return -1;

		if (opt->schema)
			break;
	}

	return count;
}

static int parse_positional_arg(struct option_parser* self, char argc,
		const char* const* argv, int i)
{
	const struct wv_option* opt = find_positional_option(self, self->position);
	if (!opt)
		return 1;

	if (append_value(self, opt, argv[i]) < 0)
		return -1;

	self->position += 1;

	return opt->is_subcommand ? 0 : 1;
}

int option_parser_parse(struct option_parser* self, int argc,
		const char* const* argv)
{
	int i = 1;
	while (i < argc) {
		if (argv[i][0] == '-') {
			if (argv[i][1] == '-') {
				if (argv[i][2] == '\0')
					return 0;

				int rc = parse_long_arg(self, argc, argv, i);
				if (rc < 0)
					return -1;
				i += rc;
			} else {
				int rc = parse_short_args(self, argc, argv, i);
				if (rc < 0)
					return -1;
				i += rc;
			}
		} else {
			int rc = parse_positional_arg(self, argc, argv, i);
			if (rc < 0)
				return -1;
			if (rc == 0)
				break;
			i += rc;
		}
	}
	self->endpos = i;
	return 0;
}

const char* option_parser_get_value(const struct option_parser* self,
		const char* name)
{
	const struct wv_option* opt;

	bool is_short = name[0] && !name[1];

	for (int i = 0; i < self->n_values; ++i) {
		const struct wv_option_value* value = &self->values[i];
		opt = value->option;

		if (is_short) {
			if (opt->short_opt && opt->short_opt == *name)
				return value->value;
		} else {
			if (opt->long_opt && strcmp(opt->long_opt, name) == 0)
				return value->value;
		}

		if (opt->positional && strcmp(opt->positional, name) == 0)
			return value->value;
	}

	if (is_short) {
		opt = find_short_option(self, name[0]);
		if (opt)
			return opt->default_;
	} else {
		opt = find_long_option(self, name);
		if (opt)
			return opt->default_;
	}

	// TODO: Add positional option?

	return NULL;
}
