#define _POSIX_C_SOURCE 200809L

#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Width of the int field on disk. 11 chars covers INT32_MIN ("-2147483648")
 * plus '\n' fits in PIPE_BUF, so pwrite is atomic. */
#define VALUE_FIELD_WIDTH 11

static int mkdir_one(const char *path)
{
	if (mkdir(path, 0775) != 0 && errno != EEXIST) {
		fprintf(stderr, "shm: mkdir %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

int shm_base_init(void)
{
	return mkdir_one(SHM_BASE_DIR);
}

static int build_attr_path(char *out, size_t out_sz,
                           int slot, int channel, const char *attr)
{
	int n;
	if (channel < 0)
		n = snprintf(out, out_sz, "%s/slot%d/%s", SHM_BASE_DIR, slot, attr);
	else
		n = snprintf(out, out_sz, "%s/slot%d/channel%d/%s",
		             SHM_BASE_DIR, slot, channel + 1, attr);
	if (n < 0 || (size_t)n >= out_sz) {
		fprintf(stderr, "shm: path too long for slot=%d ch=%d attr=%s\n",
		        slot, channel, attr);
		return -1;
	}
	return 0;
}

static int ensure_parent_dirs(int slot, int channel)
{
	char path[128];

	int n = snprintf(path, sizeof(path), "%s/slot%d", SHM_BASE_DIR, slot);
	if (n < 0 || (size_t)n >= sizeof(path)) return -1;
	if (mkdir_one(path) != 0) return -1;

	if (channel >= 0) {
		n = snprintf(path, sizeof(path), "%s/slot%d/channel%d",
		             SHM_BASE_DIR, slot, channel + 1);
		if (n < 0 || (size_t)n >= sizeof(path)) return -1;
		if (mkdir_one(path) != 0) return -1;
	}
	return 0;
}

int shm_open_attr(int slot, int channel, const char *attr, int open_flags)
{
	if (ensure_parent_dirs(slot, channel) != 0) return -1;

	char path[160];
	if (build_attr_path(path, sizeof(path), slot, channel, attr) != 0) return -1;

	int fd = open(path, open_flags, 0664);
	if (fd < 0) {
		fprintf(stderr, "shm: open %s (flags=0x%x): %s\n",
		        path, open_flags, strerror(errno));
		return -1;
	}

	if (open_flags & O_CREAT) {
		char buf[VALUE_FIELD_WIDTH + 1];
		memset(buf, ' ', VALUE_FIELD_WIDTH);
		buf[VALUE_FIELD_WIDTH] = '\n';
		(void)pwrite(fd, buf, sizeof(buf), 0);
	}
	return fd;
}

void shm_write_int(int fd, int32_t value)
{
	char buf[VALUE_FIELD_WIDTH + 1];
	int n = snprintf(buf, sizeof(buf), "%*d", VALUE_FIELD_WIDTH, value);
	if (n != VALUE_FIELD_WIDTH) return;
	buf[VALUE_FIELD_WIDTH] = '\n';
	(void)pwrite(fd, buf, sizeof(buf), 0);
}

int shm_read_int(int fd, int32_t *out)
{
	char buf[32];
	ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0) return -1;
	buf[n] = '\0';

	char *end;
	errno = 0;
	long v = strtol(buf, &end, 10);
	if (errno != 0 || end == buf) return -1;
	if (v < INT32_MIN || v > INT32_MAX) return -1;
	*out = (int32_t)v;
	return 0;
}

int shm_alias_create(const char *name, int slot, int channel)
{
	if (!name || name[0] == '\0') return 0;

	for (const char *p = name; *p; p++) {
		char c = *p;
		int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		         (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) {
			fprintf(stderr, "shm: alias \"%s\" has invalid char '%c' "
			                "(only [A-Za-z0-9_-] allowed)\n", name, c);
			return -1;
		}
	}
	if (strncmp(name, "slot", 4) == 0) {
		fprintf(stderr, "shm: alias \"%s\" reserved (collides with slotN/)\n", name);
		return -1;
	}

	char link_path[160], target[64];
	int n = snprintf(link_path, sizeof(link_path), "%s/%s", SHM_BASE_DIR, name);
	if (n < 0 || (size_t)n >= sizeof(link_path)) return -1;
	n = snprintf(target, sizeof(target), "slot%d/channel%d/value", slot, channel + 1);
	if (n < 0 || (size_t)n >= sizeof(target)) return -1;

	(void)unlink(link_path);
	if (symlink(target, link_path) != 0) {
		fprintf(stderr, "shm: symlink %s -> %s: %s\n",
		        link_path, target, strerror(errno));
		return -1;
	}
	return 0;
}
