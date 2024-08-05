#include "tst.h"
#include "table-printer.h"
#include <stdlib.h>

static int test_reflow_text(void)
{
	char buf[20];
	const char* src = "one two three four";
	int len;

	len = table_printer_reflow_text(buf, sizeof(buf), src, 20);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two three four", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 18);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two three four", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 17);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two three\nfour", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 10);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two\nthree four", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 8);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two\nthree\nfour", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 7);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one two\nthree\nfour", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 6);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one\ntwo\nthree\nfour", buf);

	len = table_printer_reflow_text(buf, sizeof(buf), src, 5);
	ASSERT_INT_EQ(18, len);
	ASSERT_STR_EQ("one\ntwo\nthree\nfour", buf);

	// width <= 4 cause aborts (if any word length > width)

	return 0;
}

static int test_reflow_multiline(void)
{
	char buf[20];
	const char* src = "one two\nthree four";

	table_printer_reflow_text(buf, sizeof(buf), src, 20);
	ASSERT_STR_EQ("one two\nthree four", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 18);
	ASSERT_STR_EQ("one two\nthree four", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 17);
	ASSERT_STR_EQ("one two\nthree four", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 10);
	ASSERT_STR_EQ("one two\nthree four", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 9);
	ASSERT_STR_EQ("one two\nthree\nfour", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 7);
	ASSERT_STR_EQ("one two\nthree\nfour", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 6);
	ASSERT_STR_EQ("one\ntwo\nthree\nfour", buf);

	table_printer_reflow_text(buf, sizeof(buf), src, 5);
	ASSERT_STR_EQ("one\ntwo\nthree\nfour", buf);

	return 0;
}

static int test_indent_and_reflow(void)
{
	size_t len;
	char* buf;
	FILE* stream;

	stream = open_memstream(&buf, &len);
	table_printer_indent_and_reflow_text(stream, "one two three four", 7, 2, 4);
	fclose(stream);
	// strlen(src)=18 + first=2 + subsequent=(2x4) + newline=1
	ASSERT_INT_EQ(29, len);
	ASSERT_STR_EQ("  one two\n    three\n    four\n", buf);
	free(buf);
	return 0;
}

static int test_defaults(void)
{
	struct table_printer one;
	table_printer_init(&one, stdout);
	table_printer_set_defaults(20, 2, 2);
	struct table_printer two;
	table_printer_init(&two, stderr);
	ASSERT_INT_EQ(80, one.max_width);
	ASSERT_INT_EQ(4, one.left_indent);
	ASSERT_INT_EQ(4, one.column_offset);
	ASSERT_INT_EQ(30, one.left_width);
	ASSERT_PTR_EQ(stdout, one.stream);
	ASSERT_INT_EQ(20, two.max_width);
	ASSERT_INT_EQ(2, two.left_indent);
	ASSERT_INT_EQ(2, two.column_offset);
	ASSERT_INT_EQ(30, two.left_width);
	ASSERT_PTR_EQ(stderr, two.stream);
	return 0;
}

static int test_print_line(void)
{
	size_t len;
	char* buf;
	struct table_printer printer = {
		.max_width = 20,
		.left_indent = 2,
		.left_width = 10,
		.column_offset = 2,
	};

	printer.stream = open_memstream(&buf, &len);
	table_printer_print_line(&printer, "left", "right");
	fclose(printer.stream);
	ASSERT_STR_EQ("  left    right\n", buf);
	free(buf);

	printer.stream = open_memstream(&buf, &len);
	table_printer_print_line(&printer, "left", "right side will wrap");
	fclose(printer.stream);
	ASSERT_STR_EQ("  left    right side\n"
		      "          will wrap\n", buf);
	free(buf);
	return 0;
}

static int test_print_fmtline(void)
{
	size_t len;
	char* buf;
	struct table_printer printer = {
		.max_width = 25,
		.left_indent = 2,
		.left_width = 15,
		.column_offset = 2,
	};

	printer.stream = open_memstream(&buf, &len);
	table_printer_print_fmtline(&printer, "right", "left");
	fclose(printer.stream);
	ASSERT_STR_EQ("  left         right\n", buf);
	free(buf);

	printer.stream = open_memstream(&buf, &len);
	table_printer_print_fmtline(&printer, "right side will wrap", "left%d", 2);
	fclose(printer.stream);
	ASSERT_STR_EQ("  left2        right side\n"
		      "               will wrap\n", buf);
	free(buf);
	return 0;
}

int main()
{
	int r = 0;
	RUN_TEST(test_reflow_text);
	RUN_TEST(test_reflow_multiline);
	RUN_TEST(test_indent_and_reflow);
	RUN_TEST(test_defaults);
	RUN_TEST(test_print_line);
	RUN_TEST(test_print_fmtline);
	return r;
}
