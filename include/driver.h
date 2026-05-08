#ifndef DRIVER_H
#define DRIVER_H

#include "config.h"

struct driver;

struct driver_ops {
	const char *type;     /* must match slot_cfg.module_type */

	/* Allocate state, parse module-specific JSON, configure module via SPI,
	   open shm files. Returns 0 on success. */
	int  (*init)(struct driver *drv, const struct slot_cfg *cfg);

	/* One iteration of the cyclic loop. Returns 0 on success;
	   non-fatal SPI errors should be logged but should not stop the daemon. */
	int  (*tick)(struct driver *drv);

	/* Release resources. Must tolerate partial init. */
	void (*shutdown)(struct driver *drv);
};

struct driver {
	const struct driver_ops *ops;
	const struct slot_cfg   *cfg;   /* points into config_file, do not free */
	void                    *state; /* per-driver private state */
};

#endif
