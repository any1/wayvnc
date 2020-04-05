/* The following is based on code puublished under public domain by Drew DeVault
 * here: https://wayland-book.com/book/surfaces/shared-memory.html
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

// Linux with glibc < 2.27 has no wrapper
#if defined(HAVE_MEMFD) && !defined(HAVE_MEMFD_CREATE)
#include <sys/syscall.h>

static inline int memfd_create(const char *name, unsigned int flags) {
	return syscall(SYS_memfd_create, name, flags);
}
#endif

#ifndef HAVE_MEMFD
static void randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;

	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}
#endif

static int create_shm_file(void)
{
#ifdef HAVE_MEMFD
	return memfd_create("wayvnc-shm", 0);
#elif defined(__FreeBSD__)
	// memfd_create added in FreeBSD 13, but SHM_ANON has been supported for ages
	return shm_open(SHM_ANON, O_RDWR | O_CREAT | O_EXCL, 0600);
#else
	int retries = 100;

	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;

		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
#endif
}

int shm_alloc_fd(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;

	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		close(fd);
		return -1;
	}

	return fd;
}
