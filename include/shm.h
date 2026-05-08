#ifndef SHM_H
#define SHM_H

#include <stdbool.h>
#include <stdint.h>

#define SHM_BASE_DIR "/dev/shm/gocontroll"

/* Create base directory /dev/shm/gocontroll. Idempotent. */
int  shm_base_init(void);

/* Open (and create) /dev/shm/gocontroll/slot{slot}/channel{ch}/{attr}.
 * channel is 0-based; pass -1 for a slot-level attribute (no channelN directory).
 * Parent directories are created with mode 0775.
 *
 * mode: O_RDONLY for files written by host (output commands),
 *       O_WRONLY|O_CREAT|O_TRUNC for files written by driver.
 *
 * Returns the fd, or -1 on error. */
int  shm_open_attr(int slot, int channel, const char *attr, int open_flags);

/* Write one int as "%d\n" atomically at offset 0. Pads with spaces so the
 * file size never grows or shrinks across writes. */
void shm_write_int(int fd, int32_t value);

/* Read int from offset 0. Returns 0 on success, -1 on parse failure. */
int  shm_read_int(int fd, int32_t *out);

/* Create symlink /dev/shm/gocontroll/<name> -> slot{slot}/channel{ch}/value.
 * If a symlink with that name already exists, it is replaced. */
int  shm_alias_create(const char *name, int slot, int channel);

#endif
