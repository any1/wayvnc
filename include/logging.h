#pragma once

#include <stdio.h>

#ifdef NDEBUG
#define log_debug(...)
#else
#define log_debug(...) fprintf(stderr, "DEBUG: " __VA_ARGS__)
#endif

#define log_error(...) fprintf(stderr, "ERROR: " __VA_ARGS__)
