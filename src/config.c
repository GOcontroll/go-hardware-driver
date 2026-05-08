#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json.h>

static int copy_str(char *dst, size_t dst_sz, const char *src)
{
	size_t n = strlen(src);
	if (n >= dst_sz) return -1;
	memcpy(dst, src, n + 1);
	return 0;
}

static int read_str(struct json_object *obj, const char *key,
                    char *out, size_t out_sz, int required)
{
	struct json_object *v;
	if (!json_object_object_get_ex(obj, key, &v)) {
		if (required) {
			fprintf(stderr, "config: missing string \"%s\"\n", key);
			return -1;
		}
		out[0] = '\0';
		return 0;
	}
	if (!json_object_is_type(v, json_type_string)) {
		fprintf(stderr, "config: \"%s\" is not a string\n", key);
		return -1;
	}
	if (copy_str(out, out_sz, json_object_get_string(v)) != 0) {
		fprintf(stderr, "config: \"%s\" too long\n", key);
		return -1;
	}
	return 0;
}

static int read_int(struct json_object *obj, const char *key,
                    int64_t *out, int required)
{
	struct json_object *v;
	if (!json_object_object_get_ex(obj, key, &v)) {
		if (required) {
			fprintf(stderr, "config: missing int \"%s\"\n", key);
			return -1;
		}
		*out = 0;
		return 0;
	}
	if (!json_object_is_type(v, json_type_int)) {
		fprintf(stderr, "config: \"%s\" is not an int\n", key);
		return -1;
	}
	*out = json_object_get_int64(v);
	return 0;
}

static int parse_slot(struct json_object *obj, struct slot_cfg *out)
{
	int64_t slot, article;

	if (read_int(obj, "slot", &slot, 1) != 0) return -1;
	if (slot < 1 || slot > 8) {
		fprintf(stderr, "config: slot %lld out of range 1..8\n", (long long)slot);
		return -1;
	}
	out->slot = (int)slot;

	if (read_str(obj, "module_type", out->module_type, sizeof(out->module_type), 1) != 0) return -1;

	if (read_int(obj, "article_number", &article, 1) != 0) return -1;
	if (article < 0 || article > UINT32_MAX) {
		fprintf(stderr, "config: slot %d article_number out of range\n", out->slot);
		return -1;
	}
	out->article_number = (uint32_t)article;

	if (read_str(obj, "hardware_version", out->hardware_version, sizeof(out->hardware_version), 1) != 0) return -1;
	if (read_str(obj, "firmware_version", out->firmware_version, sizeof(out->firmware_version), 1) != 0) return -1;
	if (read_str(obj, "label",            out->label,            sizeof(out->label),            0) != 0) return -1;

	struct json_object *m;
	out->module_obj = NULL;
	if (json_object_object_get_ex(obj, "module", &m)) {
		if (!json_object_is_type(m, json_type_object)) {
			fprintf(stderr, "config: slot %d \"module\" is not an object\n", out->slot);
			return -1;
		}
		out->module_obj = m;
	}

	struct json_object *ch;
	out->channels_obj = NULL;
	if (json_object_object_get_ex(obj, "channels", &ch)) {
		if (!json_object_is_type(ch, json_type_array)) {
			fprintf(stderr, "config: slot %d \"channels\" is not an array\n", out->slot);
			return -1;
		}
		out->channels_obj = ch;
	}

	return 0;
}

int config_load(const char *path, struct config_file *out)
{
	memset(out, 0, sizeof(*out));

	struct json_object *root = json_object_from_file(path);
	if (!root) {
		fprintf(stderr, "config: cannot read/parse %s\n", path);
		return -1;
	}
	out->root = root;

	if (read_str(root, "schema_version", out->schema_version, sizeof(out->schema_version), 1) != 0) goto fail;

	if (out->schema_version[0] != '1' || out->schema_version[1] != '.') {
		fprintf(stderr, "config: schema_version \"%s\" not supported (need 1.x)\n",
		        out->schema_version);
		goto fail;
	}

	if (read_str(root, "controller", out->controller, sizeof(out->controller), 1) != 0) goto fail;

	struct json_object *slots_arr;
	if (!json_object_object_get_ex(root, "slots", &slots_arr) ||
	    !json_object_is_type(slots_arr, json_type_array)) {
		fprintf(stderr, "config: \"slots\" missing or not an array\n");
		goto fail;
	}

	size_t n = json_object_array_length(slots_arr);
	if (n == 0) {
		fprintf(stderr, "config: no slots configured\n");
		goto fail;
	}

	out->slots = calloc(n, sizeof(*out->slots));
	if (!out->slots) {
		fprintf(stderr, "config: calloc: %s\n", strerror(errno));
		goto fail;
	}
	out->n_slots = n;

	for (size_t i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(slots_arr, i);
		if (!json_object_is_type(e, json_type_object)) {
			fprintf(stderr, "config: slots[%zu] is not an object\n", i);
			goto fail;
		}
		if (parse_slot(e, &out->slots[i]) != 0) goto fail;
	}

	for (size_t i = 0; i < n; i++)
		for (size_t j = i + 1; j < n; j++)
			if (out->slots[i].slot == out->slots[j].slot) {
				fprintf(stderr, "config: duplicate slot %d\n", out->slots[i].slot);
				goto fail;
			}

	return 0;

fail:
	config_free(out);
	return -1;
}

void config_free(struct config_file *cf)
{
	if (!cf) return;
	free(cf->slots);
	cf->slots = NULL;
	cf->n_slots = 0;
	if (cf->root) {
		json_object_put(cf->root);
		cf->root = NULL;
	}
}
