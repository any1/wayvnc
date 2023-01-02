#include "tst.h"

#include "option-parser.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const struct wv_option options[] = {
	{ .positional = "first" },
	{ .positional = "second" },
	{ .positional = "third" },
	{ .positional = "command", .is_subcommand = true },
	{ 'a', "option-a", NULL, "Description of a" },
	{ 'b', "option-b", NULL, "Description of b" },
	{ 'v', "value-option", "value", "Description of v" },
	{ },
};

static int test_simple(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);

	const char* argv[] = {
		"executable",
		"-a",
		"-b",
		"pos 1",
		"pos 2",
	};

	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("pos 1", option_parser_get_value(&parser, "first"));
	ASSERT_STR_EQ("pos 2", option_parser_get_value(&parser, "second"));
	ASSERT_FALSE(option_parser_get_value(&parser, "third"));
	ASSERT_TRUE(option_parser_get_value(&parser, "a"));
	ASSERT_TRUE(option_parser_get_value(&parser, "option-b"));
	ASSERT_FALSE(option_parser_get_value(&parser, "value-option"));
	ASSERT_INT_EQ(0, parser.remaining_argc);
	ASSERT_FALSE(parser.remaining_argv);

	return 0;
}

static int test_extra_positional_args(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);

	const char* argv[] = {
		"executable",
		"pos 1",
		"pos 2",
		"-a",
		"pos 3",
		"-b",
		"pos 4",
	};

	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("pos 1", option_parser_get_value(&parser, "first"));
	ASSERT_STR_EQ("pos 2", option_parser_get_value(&parser, "second"));
	ASSERT_STR_EQ("pos 3", option_parser_get_value(&parser, "third"));
	ASSERT_TRUE(option_parser_get_value(&parser, "a"));
	ASSERT_TRUE(option_parser_get_value(&parser, "option-b"));
	ASSERT_FALSE(option_parser_get_value(&parser, "value-option"));
	ASSERT_INT_EQ(1, parser.remaining_argc);
	ASSERT_STR_EQ("pos 4", parser.remaining_argv[0]);

	return 0;
}
static int test_short_value_option_with_space(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-v", "value" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("value", option_parser_get_value(&parser, "value-option"));
	return 0;
}

static int test_short_value_option_without_space(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-vvalue" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("value", option_parser_get_value(&parser, "value-option"));
	return 0;
}

static int test_short_value_option_with_eq(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-v=value" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("value", option_parser_get_value(&parser, "value-option"));
	return 0;
}

static int test_long_value_option_with_space(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "--value-option", "value" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("value", option_parser_get_value(&parser, "value-option"));
	return 0;
}

static int test_long_value_option_without_space(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "--value-option=value" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_STR_EQ("value", option_parser_get_value(&parser, "value-option"));
	return 0;
}

static int test_multi_short_option(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-ab" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_TRUE(option_parser_get_value(&parser, "a"));
	ASSERT_TRUE(option_parser_get_value(&parser, "b"));
	return 0;
}

static int test_multi_short_option_with_value(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-abvthe-value" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_TRUE(option_parser_get_value(&parser, "a"));
	ASSERT_TRUE(option_parser_get_value(&parser, "b"));
	ASSERT_STR_EQ("the-value", option_parser_get_value(&parser, "v"));
	return 0;
}

static int test_stop(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "exec", "-a", "--", "-b"};
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));

	ASSERT_TRUE(option_parser_get_value(&parser, "a"));
	ASSERT_FALSE(option_parser_get_value(&parser, "b"));
	ASSERT_INT_EQ(1, parser.remaining_argc);
	ASSERT_STR_EQ("-b", parser.remaining_argv[0]);
	return 0;
}

static int test_unknown_short_option(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-x" };
	ASSERT_INT_EQ(-1, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	return 0;
}

static int test_unknown_long_option(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "--an-unknown-long-option" };
	ASSERT_INT_EQ(-1, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	return 0;
}

static int test_missing_short_value(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-v" };
	ASSERT_INT_EQ(-1, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	return 0;
}

static int test_missing_long_value(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "--value-option" };
	ASSERT_INT_EQ(-1, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	return 0;
}

static int test_subcommand_without_arguments(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-ab", "first", "second", "third",
		"do-stuff" };
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	ASSERT_STR_EQ("do-stuff", option_parser_get_value(&parser, "command"));
	ASSERT_INT_EQ(1, parser.remaining_argc);
	ASSERT_STR_EQ("do-stuff", parser.remaining_argv[0]);
	return 0;
}

static int test_subcommand_with_arguments(void)
{
	struct option_parser parser;
	option_parser_init(&parser, options);
	const char* argv[] = { "executable", "-ab", "first", "second", "third",
		"do-stuff", "--some-option", "another-argument"};
	ASSERT_INT_EQ(0, option_parser_parse(&parser, ARRAY_SIZE(argv), argv));
	ASSERT_STR_EQ("do-stuff", option_parser_get_value(&parser, "command"));
	ASSERT_INT_EQ(3, parser.remaining_argc);
	ASSERT_STR_EQ("do-stuff", parser.remaining_argv[0]);
	ASSERT_STR_EQ("another-argument", parser.remaining_argv[2]);
	return 0;
}

int main()
{
	int r = 0;
	RUN_TEST(test_simple);
	RUN_TEST(test_extra_positional_args);
	RUN_TEST(test_short_value_option_with_space);
	RUN_TEST(test_short_value_option_without_space);
	RUN_TEST(test_short_value_option_with_eq);
	RUN_TEST(test_long_value_option_with_space);
	RUN_TEST(test_long_value_option_without_space);
	RUN_TEST(test_multi_short_option);
	RUN_TEST(test_multi_short_option_with_value);
	RUN_TEST(test_stop);
	RUN_TEST(test_unknown_short_option);
	RUN_TEST(test_unknown_long_option);
	RUN_TEST(test_missing_short_value);
	RUN_TEST(test_missing_long_value);
	RUN_TEST(test_subcommand_without_arguments);
	RUN_TEST(test_subcommand_with_arguments);
	return r;
}
