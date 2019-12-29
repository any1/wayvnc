#include <time.h>
#include <stdint.h>

static inline uint64_t gettime_us(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ULL + (double)ts.tv_nsec / 1000ULL;
}

static inline uint32_t gettime_ms(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
}
