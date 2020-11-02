#pragma once

#include <stdbool.h>

bool pam_auth(const char* username, const char* password);
