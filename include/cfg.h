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

#include <stdbool.h>
#include <stdint.h>

#define X_CFG_LIST \
	X(bool, enable_auth) \
	X(string, private_key_file) \
	X(string, certificate_file) \
	X(string, username) \
	X(string, password) \
	X(string, address) \
	X(uint, port) \
	X(bool, enable_pam) \
            
struct cfg {
#define string char*
#define uint uint32_t
#define X(type, name) type name;
	X_CFG_LIST
#undef X
#undef uint
#undef string
};

int cfg_load(struct cfg* self, const char* path);
void cfg_destroy(struct cfg* self);
