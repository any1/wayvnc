/*
 * Copyright (c) 2023 Andri Yngvason
 * Copyright (c) 2023 Jim Ramsay
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

#include "table-printer.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

static struct table_printer defaults = {
	.max_width = 80,
	.left_indent = 4,
	.column_offset = 8,
	.stream = NULL,
	.left_width = 0,
};

void table_printer_set_defaults(int max_width, int left_indent,
		int column_offset)
{
	defaults.max_width = max_width;
	defaults.left_indent = left_indent;
	defaults.column_offset = column_offset;
}

void table_printer_init(struct table_printer* self, FILE* stream,
		int left_width)
{
	memcpy(self, &defaults, sizeof(*self));
	self->stream = stream;
	self->left_width = left_width;
}

int table_printer_reflow_text(char* dst, int dst_size, const char* src,
		int width)
{
	int line_len = 0;
	int last_space_pos = 0;

	int dst_len = 0;
	int i = 0;

	while (true) {
		char c = src[i];
		if (line_len > width) {
			// first word > width
			assert(last_space_pos > 0);
			// subsequent word > width
			assert(dst[last_space_pos] != '\n');

			dst_len -= i - last_space_pos;
			dst[dst_len++] = '\n';
			i = last_space_pos + 1;
			line_len = 0;
			continue;
		}
		if (!c)
			break;

		if (c == ' ')
			last_space_pos = i;
		dst[dst_len++] = c;
		assert(dst_len < dst_size);
		++line_len;
		++i;

		if (c == '\n')
			line_len = 0;
	}

	dst[dst_len] = '\0';
	return dst_len;
}

void table_printer_indent_and_reflow_text(FILE* stream, const char* src,
		int width, int first_line_indent, int subsequent_indent)
{
	char buffer[256];
	table_printer_reflow_text(buffer, sizeof(buffer), src, width);

	char* line = strtok(buffer, "\n");
	fprintf(stream, "%*s%s\n", first_line_indent, "", line);

	while (true) {
		line = strtok(NULL, "\n");
		if (!line)
			break;

		fprintf(stream, "%*s%s\n", subsequent_indent, "", line);
	}
}

void table_printer_print_line(struct table_printer* self, const char* left_text,
		const char* right_text)
{
	fprintf(self->stream, "%*s", self->left_indent, "");
	int field_len = fprintf(self->stream, "%s", left_text);
	fprintf(self->stream, "%*s", self->left_width - field_len + self->column_offset, "");
	int column_indent = self->left_indent + self->left_width + self->column_offset;
	int column_width = self->max_width - column_indent;
	table_printer_indent_and_reflow_text(self->stream,
			right_text,
			column_width, 0, column_indent);
}

void table_printer_print_fmtline(struct table_printer* self,
		const char* right_text,
		const char* left_format, ...)
{
	char buf[64];
	va_list args;
	va_start(args, left_format);
	vsnprintf(buf, sizeof(buf), left_format, args);
	va_end(args);
	table_printer_print_line(self, buf, right_text);
}

