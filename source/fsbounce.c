#include <3ds.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* FSFILE_Read/Write on a real 3DS must target linear memory.
   A heap buffer makes the fs sysmodule prefetch-abort. Citra does not. */
#define BOUNCE_SIZE (16 * 1024)

static uint8_t* s_bounce;

static uint8_t* bounce(void) {
	if (!s_bounce) s_bounce = linearMemAlign(BOUNCE_SIZE, 0x1000);
	return s_bounce;
}

ssize_t __real_read(int fd, void* buf, size_t count);
ssize_t __real_write(int fd, const void* buf, size_t count);

ssize_t __wrap_read(int fd, void* buf, size_t count) {
	uint8_t* b = bounce();
	if (!b || !buf) return __real_read(fd, buf, count);

	size_t done = 0;
	while (done < count) {
		size_t chunk = count - done;
		if (chunk > BOUNCE_SIZE) chunk = BOUNCE_SIZE;
		ssize_t n = __real_read(fd, b, chunk);
		if (n < 0) return done ? (ssize_t)done : n;
		if (n == 0) break;
		memcpy((uint8_t*)buf + done, b, (size_t)n);
		done += (size_t)n;
		if ((size_t)n < chunk) break;
	}
	return (ssize_t)done;
}

ssize_t __wrap_write(int fd, const void* buf, size_t count) {
	uint8_t* b = bounce();
	if (!b || !buf) return __real_write(fd, buf, count);

	size_t done = 0;
	while (done < count) {
		size_t chunk = count - done;
		if (chunk > BOUNCE_SIZE) chunk = BOUNCE_SIZE;
		memcpy(b, (const uint8_t*)buf + done, chunk);
		ssize_t n = __real_write(fd, b, chunk);
		if (n < 0) return done ? (ssize_t)done : n;
		if (n == 0) break;
		done += (size_t)n;
		if ((size_t)n < chunk) break;
	}
	return (ssize_t)done;
}
