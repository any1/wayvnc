#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <security/pam_appl.h>

#include "logging.h"

struct credentials {
	const char* user;
	const char* password;
};

bool pam_auth(const char* username, const char* password);
