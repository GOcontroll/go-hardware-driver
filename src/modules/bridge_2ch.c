#include <stdio.h>

#include "driver.h"

static int stub_init(struct driver *drv, const struct slot_cfg *cfg)
{
	(void)drv;
	fprintf(stderr, "bridge-2ch: slot %d — driver not implemented yet\n", cfg->slot);
	return -1;
}

static int  stub_tick(struct driver *drv)     { (void)drv; return 0; }
static void stub_shutdown(struct driver *drv) { (void)drv; }

const struct driver_ops bridge_2ch_ops = {
	.type     = "bridge-2ch",
	.init     = stub_init,
	.tick     = stub_tick,
	.shutdown = stub_shutdown,
};
