#include "registry.h"

#include <string.h>

extern const struct driver_ops input_6ch_ops;
extern const struct driver_ops input_10ch_ops;
extern const struct driver_ops input_4_20ma_ops;
extern const struct driver_ops output_6ch_ops;
extern const struct driver_ops output_10ch_ops;
extern const struct driver_ops bridge_2ch_ops;

static const struct driver_ops *const TABLE[] = {
	&input_6ch_ops,
	&input_10ch_ops,
	&input_4_20ma_ops,
	&output_6ch_ops,
	&output_10ch_ops,
	&bridge_2ch_ops,
};

const struct driver_ops *registry_lookup(const char *module_type)
{
	if (!module_type) return NULL;
	for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++)
		if (strcmp(TABLE[i]->type, module_type) == 0)
			return TABLE[i];
	return NULL;
}
