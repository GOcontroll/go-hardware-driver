#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json.h>

#include "GO_board.h"
#include "GO_communication_modules.h"
#include "GO_module_input.h"

#include "driver.h"
#include "shm.h"

#define NUM_CHANNELS 6

struct ch_cfg {
	uint8_t  func;
	uint8_t  voltage_range;
	uint8_t  pull_up;
	uint8_t  pull_down;
	uint8_t  pulses_per_rotation;
	uint16_t analog_filter_samples;
	int      value_fd;
	int      reset_value_fd;
	int      reset_trigger_fd;
	char     name[32];
};

struct state {
	_inputModule  module;
	uint8_t       supply[3];
	struct ch_cfg ch[NUM_CHANNELS];
	int           slot;
	uint64_t      tick_count;
	uint64_t      err_count;
};

struct str_enum { const char *name; uint8_t value; };

static const struct str_enum FUNC_MAP[] = {
	{ "disabled",      0                      },  /* sentinel: skip configure */
	{ "12bit_adc",     INPUTFUNC_12BITADC     },
	{ "mv_analog",     INPUTFUNC_MVANALOG     },
	{ "digital_in",    INPUTFUNC_DIGITAL_IN   },
	{ "frequency",     INPUTFUNC_FREQUENCY    },
	{ "duty_low",      INPUTFUNC_DUTY_LOW     },
	{ "duty_high",     INPUTFUNC_DUTY_HIGH    },
	{ "rpm",           INPUTFUNC_RPM          },
	{ "pulse_counter", INPUTFUNC_PULSECOUNTER },
};
static const struct str_enum RANGE_MAP[] = {
	{ "5V",  INPUTVOLTAGERANGE_5V  },
	{ "12V", INPUTVOLTAGERANGE_12V },
	{ "24V", INPUTVOLTAGERANGE_24V },
};
static const struct str_enum PULLUP_MAP[] = {
	{ "none", INPUTPULLUP_NULL    },
	{ "3_3k", INPUTPULLUP6CH_3_3K },
	{ "4_7k", INPUTPULLUP6CH_4_7K },
	{ "10k",  INPUTPULLUP6CH_10K  },
};
static const struct str_enum PULLDOWN_MAP[] = {
	{ "none", INPUTPULLDOWN_NULL    },
	{ "3_3k", INPUTPULLDOWN6CH_3_3K },
	{ "4_7k", INPUTPULLDOWN6CH_4_7K },
	{ "10k",  INPUTPULLDOWN6CH_10K  },
};
static const struct str_enum SUPPLY_MAP[] = {
	{ "off", INPUTSENSSUPPLYOFF },
	{ "on",  INPUTSENSSUPPLYON  },
};

static int lookup_enum(const struct str_enum *map, size_t n, const char *s, uint8_t *out)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(map[i].name, s) == 0) { *out = map[i].value; return 0; }
	return -1;
}
#define LOOKUP(map, s, out) lookup_enum((map), sizeof(map)/sizeof((map)[0]), (s), (out))

static int read_str(struct json_object *obj, const char *key, const char **out)
{
	struct json_object *v;
	if (!json_object_object_get_ex(obj, key, &v) ||
	    !json_object_is_type(v, json_type_string)) return -1;
	*out = json_object_get_string(v);
	return 0;
}

static int read_int(struct json_object *obj, const char *key, int64_t *out)
{
	struct json_object *v;
	if (!json_object_object_get_ex(obj, key, &v) ||
	    !json_object_is_type(v, json_type_int)) return -1;
	*out = json_object_get_int64(v);
	return 0;
}

static int parse_supply(struct json_object *m, uint8_t out[3])
{
	const char *keys[3] = { "sensor_supply_1", "sensor_supply_2", "sensor_supply_3" };
	out[0] = out[1] = out[2] = INPUTSENSSUPPLYOFF;
	if (!m) return 0;
	for (int i = 0; i < 3; i++) {
		const char *s;
		if (read_str(m, keys[i], &s) == 0) {
			if (LOOKUP(SUPPLY_MAP, s, &out[i]) != 0) {
				fprintf(stderr, "input-6ch: bad %s = \"%s\"\n", keys[i], s);
				return -1;
			}
		}
	}
	return 0;
}

static int parse_channel(struct json_object *ch, struct ch_cfg *out)
{
	const char *func_s = NULL;
	if (read_str(ch, "func", &func_s) != 0) {
		fprintf(stderr, "input-6ch: channel missing \"func\"\n");
		return -1;
	}
	if (LOOKUP(FUNC_MAP, func_s, &out->func) != 0) {
		fprintf(stderr, "input-6ch: bad func \"%s\"\n", func_s);
		return -1;
	}

	const char *s;
	out->voltage_range = INPUTVOLTAGERANGE_5V;
	if (read_str(ch, "voltage_range", &s) == 0 &&
	    LOOKUP(RANGE_MAP, s, &out->voltage_range) != 0) {
		fprintf(stderr, "input-6ch: bad voltage_range \"%s\"\n", s);
		return -1;
	}
	out->pull_up = INPUTPULLUP_NULL;
	if (read_str(ch, "pull_up", &s) == 0 &&
	    LOOKUP(PULLUP_MAP, s, &out->pull_up) != 0) {
		fprintf(stderr, "input-6ch: bad pull_up \"%s\"\n", s);
		return -1;
	}
	out->pull_down = INPUTPULLDOWN_NULL;
	if (read_str(ch, "pull_down", &s) == 0 &&
	    LOOKUP(PULLDOWN_MAP, s, &out->pull_down) != 0) {
		fprintf(stderr, "input-6ch: bad pull_down \"%s\"\n", s);
		return -1;
	}

	int64_t v;
	out->pulses_per_rotation = 0;
	if (read_int(ch, "pulses_per_rotation", &v) == 0) {
		if (v < 0 || v > PULSESPERROTATIONMAX) {
			fprintf(stderr, "input-6ch: pulses_per_rotation %lld out of range\n",
			        (long long)v);
			return -1;
		}
		out->pulses_per_rotation = (uint8_t)v;
	}
	out->analog_filter_samples = 0;
	if (read_int(ch, "analog_filter_samples", &v) == 0) {
		if (v < 0 || v > ANALOGSAMPLESMAX) {
			fprintf(stderr, "input-6ch: analog_filter_samples %lld out of range\n",
			        (long long)v);
			return -1;
		}
		out->analog_filter_samples = (uint16_t)v;
	}

	out->name[0] = '\0';
	if (read_str(ch, "name", &s) == 0) {
		size_t n = strlen(s);
		if (n >= sizeof(out->name)) {
			fprintf(stderr, "input-6ch: name \"%s\" too long\n", s);
			return -1;
		}
		memcpy(out->name, s, n + 1);
	}
	return 0;
}

static int parse_channels(struct json_object *arr, struct ch_cfg ch[NUM_CHANNELS])
{
	for (int i = 0; i < NUM_CHANNELS; i++) {
		ch[i].func = 0;
		ch[i].voltage_range = INPUTVOLTAGERANGE_5V;
		ch[i].pull_up = ch[i].pull_down = 0;
		ch[i].pulses_per_rotation = 0;
		ch[i].analog_filter_samples = 0;
		ch[i].value_fd = -1;
		ch[i].reset_value_fd = -1;
		ch[i].reset_trigger_fd = -1;
		ch[i].name[0] = '\0';
	}
	if (!arr) return 0;

	size_t n = json_object_array_length(arr);
	for (size_t i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(arr, i);
		int64_t cnum;
		if (read_int(e, "channel", &cnum) != 0 || cnum < 1 || cnum > NUM_CHANNELS) {
			fprintf(stderr, "input-6ch: bad channel index in entry %zu\n", i);
			return -1;
		}
		if (parse_channel(e, &ch[cnum - 1]) != 0) return -1;
	}
	return 0;
}

#define CHECK(call) do { \
	int _rc = (call); \
	if (_rc != 0) { \
		fprintf(stderr, "input-6ch: %s -> %d\n", #call, _rc); \
		return _rc; \
	} \
} while (0)

static int input_6ch_init(struct driver *drv, const struct slot_cfg *cfg)
{
	struct state *st = calloc(1, sizeof(*st));
	if (!st) {
		fprintf(stderr, "input-6ch: calloc: %s\n", strerror(errno));
		return -1;
	}
	for (int i = 0; i < NUM_CHANNELS; i++) {
		st->ch[i].value_fd = -1;
		st->ch[i].reset_value_fd = -1;
		st->ch[i].reset_trigger_fd = -1;
	}
	st->slot = cfg->slot;
	drv->state = st;

	if (parse_supply(cfg->module_obj, st->supply) != 0) goto fail;
	if (parse_channels(cfg->channels_obj, st->ch) != 0) goto fail;

	uint8_t slot0 = (uint8_t)(cfg->slot - 1);

	CHECK(GO_module_input_set_module_type(&st->module, INPUTMODULE6CHANNEL));

	if (GO_communication_modules_initialize(slot0) != 0) {
		fprintf(stderr, "input-6ch: GO_communication_modules_initialize(%u) failed\n", slot0);
		goto fail;
	}
	CHECK(GO_module_input_set_module_slot(&st->module, slot0));
	CHECK(GO_module_input_6ch_configure_supply(&st->module,
	                                           st->supply[0], st->supply[1], st->supply[2]));

	for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
		if (st->ch[c].func == 0) continue; /* disabled: leave SPI byte at 0 */
		CHECK(GO_module_input_6ch_configure_channel(&st->module, c,
		      st->ch[c].func, st->ch[c].voltage_range,
		      st->ch[c].pull_up, st->ch[c].pull_down,
		      st->ch[c].pulses_per_rotation, st->ch[c].analog_filter_samples));
	}
	CHECK(GO_module_input_configuration(&st->module));

	for (int c = 0; c < NUM_CHANNELS; c++) {
		st->ch[c].value_fd = shm_open_attr(cfg->slot, c, "value",
		                                   O_WRONLY | O_CREAT | O_TRUNC);
		st->ch[c].reset_value_fd = shm_open_attr(cfg->slot, c, "reset_value",
		                                         O_RDWR | O_CREAT | O_TRUNC);
		st->ch[c].reset_trigger_fd = shm_open_attr(cfg->slot, c, "reset_trigger",
		                                           O_RDWR | O_CREAT | O_TRUNC);
		if (st->ch[c].value_fd < 0 || st->ch[c].reset_value_fd < 0 ||
		    st->ch[c].reset_trigger_fd < 0) goto fail;
		if (st->ch[c].name[0])
			(void)shm_alias_create(st->ch[c].name, cfg->slot, c);
	}

	fprintf(stderr, "input-6ch: slot %d ready\n", cfg->slot);
	return 0;

fail:
	for (int c = 0; c < NUM_CHANNELS; c++) {
		if (st->ch[c].value_fd         >= 0) close(st->ch[c].value_fd);
		if (st->ch[c].reset_value_fd   >= 0) close(st->ch[c].reset_value_fd);
		if (st->ch[c].reset_trigger_fd >= 0) close(st->ch[c].reset_trigger_fd);
	}
	free(st);
	drv->state = NULL;
	return -1;
}

static int input_6ch_tick(struct driver *drv)
{
	struct state *st = drv->state;
	st->tick_count++;

	/* Pulse-counter / encoder reset: host writes reset_value (int32) and
	   reset_trigger (uint8). Library compares trigger against last-seen value
	   in pulscounterResetTrigger[c] and only sends an SPI reset when the byte
	   has changed — host requests another reset by changing the trigger. */
	for (int c = 0; c < NUM_CHANNELS; c++) {
		int32_t v, t;
		if (shm_read_int(st->ch[c].reset_value_fd,   &v) != 0) continue;
		if (shm_read_int(st->ch[c].reset_trigger_fd, &t) != 0) continue;
		(void)GO_module_input_reset_puls_counter(&st->module, (uint8_t)c, v,
		                                         (uint8_t)t);
	}

	int rc = GO_module_input_receive_values(&st->module);
	if (rc != 0) {
		if ((st->err_count++ % 1000) == 0)
			fprintf(stderr, "input-6ch: slot %d receive_values rc=%d (err=%llu)\n",
			        st->slot, rc, (unsigned long long)st->err_count);
		return rc;
	}
	for (int c = 0; c < NUM_CHANNELS; c++)
		shm_write_int(st->ch[c].value_fd, (int32_t)st->module.value[c]);
	return 0;
}

static void input_6ch_shutdown(struct driver *drv)
{
	struct state *st = drv->state;
	if (!st) return;
	for (int c = 0; c < NUM_CHANNELS; c++) {
		if (st->ch[c].value_fd         >= 0) close(st->ch[c].value_fd);
		if (st->ch[c].reset_value_fd   >= 0) close(st->ch[c].reset_value_fd);
		if (st->ch[c].reset_trigger_fd >= 0) close(st->ch[c].reset_trigger_fd);
	}
	free(st);
	drv->state = NULL;
}

const struct driver_ops input_6ch_ops = {
	.type     = "input-6ch",
	.init     = input_6ch_init,
	.tick     = input_6ch_tick,
	.shutdown = input_6ch_shutdown,
};
