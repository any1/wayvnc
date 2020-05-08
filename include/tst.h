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

#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define XSTR(s) STR(s)
#define STR(s) #s

#define ASSERT_TRUE(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be true\n"); \
		return 1; \
	} \
} while(0)

#define ASSERT_FALSE(expr) do { \
	if (expr) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be false\n"); \
		return 1; \
	} \
} while(0)

#define TST_ASSERT_EQ_(value, expr, type, fmt) do { \
	type expr_ = (expr); \
	if (expr_ != (value)) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be equal to " XSTR(value) "; was " fmt "\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_INT_EQ(value, expr) TST_ASSERT_EQ_(value, expr, int, "%d")
#define ASSERT_UINT_EQ(value, expr) TST_ASSERT_EQ_(value, expr, unsigned int, "%u")
#define ASSERT_INT32_EQ(value, expr) TST_ASSERT_EQ_(value, expr, int32_t, "%" PRIi32)
#define ASSERT_UINT32_EQ(value, expr) TST_ASSERT_EQ_(value, expr, uint32_t, "%" PRIu32)
#define ASSERT_DOUBLE_EQ(value, expr) TST_ASSERT_EQ_(value, expr, double, "%f")
#define ASSERT_PTR_EQ(value, expr) TST_ASSERT_EQ_(value, expr, void*, "%p")

#define TST_ASSERT_GT_(value, expr, type, fmt) do { \
	type expr_ = (expr); \
	if (!(expr_ > (value))) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be greater than " XSTR(value) "; was " fmt "\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_INT_GT(value, expr) TST_ASSERT_GT_(value, expr, int, "%d")
#define ASSERT_UINT_GT(value, expr) TST_ASSERT_GT_(value, expr, unsigned int, "%u")
#define ASSERT_INT32_GT(value, expr) TST_ASSERT_GT_(value, expr, int32_t, "%" PRIi32)
#define ASSERT_UINT32_GT(value, expr) TST_ASSERT_GT_(value, expr, uint32_t, "%" PRIu32)
#define ASSERT_DOUBLE_GT(value, expr) TST_ASSERT_GT_(value, expr, double, "%f")

#define TST_ASSERT_GE_(value, expr, type, fmt) do { \
	type expr_ = (expr); \
	if (!(expr_ >= (value))) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be greater than or equal to " XSTR(value) "; was " fmt "\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_INT_GE(value, expr) TST_ASSERT_GE_(value, expr, int, "%d")
#define ASSERT_UINT_GE(value, expr) TST_ASSERT_GE_(value, expr, unsigned int, "%u")
#define ASSERT_INT32_GE(value, expr) TST_ASSERT_GE_(value, expr, int32_t, "%" PRIi32)
#define ASSERT_UINT32_GE(value, expr) TST_ASSERT_GE_(value, expr, uint32_t, "%" PRIu32)
#define ASSERT_DOUBLE_GE(value, expr) TST_ASSERT_GE_(value, expr, double, "%f")

#define TST_ASSERT_LT_(value, expr, type, fmt) do { \
	type expr_ = (expr); \
	if (!(expr_ < (value))) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be less than " XSTR(value) "; was " fmt "\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_INT_LT(value, expr) TST_ASSERT_LT_(value, expr, int, "%d")
#define ASSERT_UINT_LT(value, expr) TST_ASSERT_LT_(value, expr, unsigned int, "%u")
#define ASSERT_INT32_LT(value, expr) TST_ASSERT_LT_(value, expr, int32_t, "%" PRIi32)
#define ASSERT_UINT32_LT(value, expr) TST_ASSERT_LT_(value, expr, uint32_t, "%" PRIu32)
#define ASSERT_DOUBLE_LT(value, expr) TST_ASSERT_LT_(value, expr, double, "%f")

#define TST_ASSERT_LE_(value, expr, type, fmt) do { \
	type expr_ = (expr); \
	if (!(expr_ <= (value))) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be less than or equal to " XSTR(value) "; was " fmt "\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_INT_LE(value, expr) TST_ASSERT_LE_(value, expr, int, "%d")
#define ASSERT_UINT_LE(value, expr) TST_ASSERT_LE_(value, expr, unsigned int, "%u")
#define ASSERT_INT32_LE(value, expr) TST_ASSERT_LE_(value, expr, int32_t, "%" PRIi32)
#define ASSERT_UINT32_LE(value, expr) TST_ASSERT_LE_(value, expr, uint32_t, "%" PRIu32)
#define ASSERT_DOUBLE_LE(value, expr) TST_ASSERT_LE_(value, expr, double, "%f")

#define ASSERT_STR_EQ(value, expr) do { \
	const char* expr_ = (expr); \
	if (strcmp(expr_, (value)) != 0) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to be " XSTR(value) "; was \"%s\"\n", \
			expr_); \
		return 1; \
	} \
} while(0)

#define ASSERT_NEQ(value, expr) do { \
	if ((expr) != (value)) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to NOT be " XSTR(value) "\n"); \
		return 1; \
	} \
} while(0)

#define ASSERT_STR_NEQ(value, expr) do { \
	if (strcmp((expr), (value)) == 0) { \
		fprintf(stderr, "FAILED " XSTR(__LINE__) ": Expected " XSTR(expr) " to NOT be " XSTR(value) "\n"); \
		return 1; \
	} \
} while(0)

#define RUN_TEST(test) do { \
	if(!(test())) \
		fprintf(stderr, XSTR(test) " passed\n"); \
	else \
		r = 1; \
} while(0);
