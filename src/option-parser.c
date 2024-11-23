/*
 * Copyright (c) 2022 - 2024 Andri Yngvason
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
#include "table-printer.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/param.h>
#include <ctype.h>

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
	self->name = "Options";
}

static const char* format_help(const struct wv_option* opt)
{
	if (!opt->default_)
		return opt->help;

	static char help_buf[256];
	snprintf(help_buf, sizeof(help_buf), "%s\nDefault: %s", opt->help, opt->default_);
	return help_buf;
}

static void format_option(struct table_printer* printer, const struct wv_option* opt)
{
	if (!opt->help || opt->positional)
		return;

	int n_chars = 0;
	char buf[64];
	if (opt->short_opt)
		n_chars += snprintf(buf + n_chars, sizeof(buf) - n_chars,
				"-%c", opt->short_opt);
	if (opt->long_opt)
		n_chars += snprintf(buf + n_chars, sizeof(buf) - n_chars,
				"%s--%s", opt->short_opt ? "," : "",
				opt->long_opt);
	if (opt->schema)
		n_chars += snprintf(buf + n_chars, sizeof(buf) - n_chars,
				"%s%s", opt->long_opt ? "=" : "", opt->schema);

	table_printer_print_line(printer, buf, format_help(opt));
}

void option_parser_print_options(struct option_parser* self, FILE* stream)
{
	fprintf(stream, "%s:\n", self->name);
	struct table_printer printer;
	table_printer_init(&printer, stream);
	for (int i = 0; i < self->n_opts; ++i)
		format_option(&printer, &self->options[i]);
}

static void print_string_tolower(FILE* stream, const char *src)
{
	for (const char* c = src; *c != '\0'; c++)
		fprintf(stream, "%c", tolower(*c));
}

void option_parser_print_usage(struct option_parser* self, FILE* stream)
{
	fprintf(stream, " [");
	print_string_tolower(stream, self->name);
	fprintf(stream, "]");
	int optional_paren_count = 0;
	int end = self->n_opts;
	for (int i = 0; i < self->n_opts; ++i) {
		const struct wv_option* opt = &self->options[i];
		if (!opt->positional)
			continue;
		if (opt->is_repeating) {
			end = i;
			break;
		}
		const char* open = "<";
		const char* close = ">";
		if (opt->default_) {
			open = "[";
			close = ""; // Closed via optional_paren_count loop below
			optional_paren_count++;
		} else {
			// Enforce there must be NO non-optional args after
			// we've processed at least one optional arg
			assert(optional_paren_count == 0);
		}
		fprintf(stream, " %s%s%s", open, opt->positional, close);
	}
	for (int i = 0; i < optional_paren_count; ++i)
		fprintf(stream, "]");

	for (int i = end; i < self->n_opts; ++i) {
		const struct wv_option* opt = &self->options[i];
		if (!opt->positional)
			continue;
		assert(opt->is_repeating);

		fprintf(stream, " [%s...]", opt->positional);
	}
}

int option_parser_print_arguments(struct option_parser* self, FILE* stream)
{
	bool have_args = 0;
	for (int i = 0; i < self->n_opts; ++i) {
		const struct wv_option* opt = &self->options[i];
		if (!opt->positional || !opt->help || opt->is_subcommand)
			continue;
		have_args = true;
	}
	if (!have_args)
		return 0;

	fprintf(stream, "Arguments:\n");
	struct table_printer printer;
	table_printer_init(&printer, stream);
	int i;
	for (i = 0; i < self->n_opts; ++i) {
		const struct wv_option* opt = &self->options[i];
		if (!opt->positional || !opt->help || opt->is_subcommand)
			continue;
		table_printer_print_line(&printer, opt->positional, format_help(opt));
	}
	return i;
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

static const struct wv_option* find_positional_option_by_name(
		const struct option_parser* self, const char*name)
{
	for (int i = 0; i < self->n_opts; ++i) {
		const struct wv_option* opt = &self->options[i];
		if (!opt->positional)
			continue;
		if (strcmp(opt->positional, name) == 0)
			return opt;
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

	if (!opt->is_repeating)
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
				if (argv[i][2] == '\0') {
					i++;
					break;
				}

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
	self->remaining_argc = argc - i;
	if (self->remaining_argc)
		self->remaining_argv = argv + i;
	return 0;
}

const char* option_parser_get_value_no_default(const struct option_parser* self,
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

	return NULL;
}

const char* option_parser_get_value(const struct option_parser* self,
		const char* name)
{
	const char* value = option_parser_get_value_no_default(self, name);
	if (value)
		return value;

	bool is_short = name[0] && !name[1];
	const struct wv_option* opt;
	if (is_short) {
		opt = find_short_option(self, name[0]);
		if (opt)
			return opt->default_;
	} else {
		opt = find_long_option(self, name);
		if (opt)
			return opt->default_;
		opt = find_positional_option_by_name(self, name);
		if (opt)
			return opt->default_;
	}

	return NULL;
}

const char* option_parser_get_value_with_offset(const struct option_parser* self,
		const char* name, int index)
{
	const struct wv_option* opt;

	for (int i = 0; i < self->n_values; ++i) {
		const struct wv_option_value* value = &self->values[i];
		opt = value->option;

		if (!opt->positional || !opt->is_repeating)
			continue;

		if (strcmp(opt->positional, name) != 0)
			continue;

		if (index-- > 0)
			continue;

		return value->value;
	}

	return NULL;
}

void option_parser_print_cmd_summary(const char* summary, FILE* stream)
{
	struct table_printer printer;
	table_printer_init(&printer, stream);
	fprintf(stream, "\n");
	table_printer_indent_and_reflow_text(stream, summary, printer.max_width, 0, 0);
	fprintf(stream, "\n");
}
