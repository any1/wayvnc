/*
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

#include <stdio.h>

struct table_printer{
	FILE* stream;
	int max_width;
	int left_indent;
	int left_width;
	int column_offset;
};

// Sets default values for every subsequent table_printer_new (Optional: defaults to 80/4/8)
void table_printer_set_defaults(int max_width, int left_indent,
		int column_offset);

void table_printer_init(struct table_printer* self, FILE* stream,
		int left_width);

void table_printer_print_line(struct table_printer* self, const char* left_text,
		const char* right_text);

void table_printer_print_fmtline(struct table_printer* self,
		const char* right_text,
		const char* left_format, ...);

int table_printer_reflow_text(char* dst, int dst_size, const char* src,
		int width);

void table_printer_indent_and_reflow_text(FILE* stream, const char* src,
		int width, int first_line_indent, int subsequent_indent);

