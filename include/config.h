#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

struct json_object;

#define CFG_TYPE_MAX     32
#define CFG_VERSION_MAX  16
#define CFG_LABEL_MAX    64
#define CFG_CONTROLLER_MAX 32

struct slot_cfg {
	int      slot;                    /* 1-based slot index from JSON */
	char     module_type[CFG_TYPE_MAX];
	uint32_t article_number;
	char     hardware_version[CFG_VERSION_MAX];
	char     firmware_version[CFG_VERSION_MAX];
	char     label[CFG_LABEL_MAX];

	/* Opaque pointers into the parsed JSON tree, owned by config_file.
	   Module drivers parse their own keys from these. May be NULL. */
	struct json_object *module_obj;
	struct json_object *channels_obj;
};

struct config_file {
	char schema_version[CFG_VERSION_MAX];
	char controller[CFG_CONTROLLER_MAX];

	struct slot_cfg *slots;
	size_t           n_slots;

	struct json_object *root;
};

/* Loads and validates the v1.0 modules.json schema. Returns 0 on success. */
int  config_load(const char *path, struct config_file *out);

/* Frees memory; safe to call on a zeroed struct. */
void config_free(struct config_file *cf);

#endif
