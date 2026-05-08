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
#include "GO_module_output.h"

#include "driver.h"
#include "shm.h"

#define NUM_CHANNELS 6
#define NUM_FREQ_PAIRS 3

struct ch_cfg {
	uint8_t  func;
	uint16_t current_max;
	uint16_t peak_current;
	uint16_t peak_time;
	int      cmd_fd;
	int      current_fd;
	int      duty_fd;
	char     name[32];
};

struct state {
	_outputModule module;
	uint8_t       freq[NUM_FREQ_PAIRS];
	struct ch_cfg ch[NUM_CHANNELS];
	int           fd_temperature;
	int           fd_ground;
	int           fd_supply;
	int           fd_error_code;
	int           slot;
	uint64_t      tick_count;
	uint64_t      err_count;
};

struct str_enum { const char *name; uint8_t value; };

static const struct str_enum FUNC_MAP[] = {
	{ "disabled",      OUTPUTFUNC_DISABLED         },
	{ "halfbridge",    OUTPUTFUNC_6CH_HALFBRIDGE   },
	{ "lowside_duty",  OUTPUTFUNC_6CH_LOWSIDEDUTY  },
	{ "highside_duty", OUTPUTFUNC_6CH_HIGHSIDEDUTY },
	{ "lowside_bool",  OUTPUTFUNC_6CH_LOWSIDEBOOL  },
	{ "highside_bool", OUTPUTFUNC_6CH_HIGHSIDEBOOL },
	{ "peak_and_hold", OUTPUTFUNC_6CH_PEAKANDHOLD  },
	{ "frequency_out", OUTPUTFUNC_6CH_FREQUENCYOUT },
};
static const struct str_enum FREQ_MAP[] = {
	{ "100Hz", OUTPUTFREQ_100HZ      },
	{ "200Hz", OUTPUTFREQ_200HZ      },
	{ "500Hz", OUTPUTFREQ_6CH_500HZ  },
	{ "1kHz",  OUTPUTFREQ_6CH_1KHZ   },
	{ "2kHz",  OUTPUTFREQ_6CH_2KHZ   },
	{ "5kHz",  OUTPUTFREQ_6CH_5KHZ   },
	{ "10kHz", OUTPUTFREQ_6CH_10KHZ  },
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

static int parse_freq_pairs(struct json_object *m, uint8_t freq[NUM_FREQ_PAIRS])
{
	freq[0] = freq[1] = freq[2] = OUTPUTFREQ_100HZ;
	if (!m) return 0;

	struct json_object *arr;
	if (!json_object_object_get_ex(m, "frequency_pairs", &arr)) return 0;
	if (!json_object_is_type(arr, json_type_array)) {
		fprintf(stderr, "output-6ch: frequency_pairs is not an array\n");
		return -1;
	}
	size_t n = json_object_array_length(arr);
	if (n > NUM_FREQ_PAIRS) n = NUM_FREQ_PAIRS;
	for (size_t i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(arr, i);
		if (!json_object_is_type(e, json_type_string)) {
			fprintf(stderr, "output-6ch: frequency_pairs[%zu] is not a string\n", i);
			return -1;
		}
		if (LOOKUP(FREQ_MAP, json_object_get_string(e), &freq[i]) != 0) {
			fprintf(stderr, "output-6ch: bad frequency \"%s\"\n",
			        json_object_get_string(e));
			return -1;
		}
	}
	return 0;
}

static int parse_channel(struct json_object *ch, struct ch_cfg *out)
{
	const char *func_s;
	if (read_str(ch, "func", &func_s) != 0) {
		fprintf(stderr, "output-6ch: channel missing \"func\"\n");
		return -1;
	}
	if (LOOKUP(FUNC_MAP, func_s, &out->func) != 0) {
		fprintf(stderr, "output-6ch: bad func \"%s\"\n", func_s);
		return -1;
	}

	int64_t v;
	out->current_max = 0;
	if (read_int(ch, "current_max", &v) == 0) {
		if (v < 0 || v > CURRENTMAXMAX) {
			fprintf(stderr, "output-6ch: current_max %lld out of range 0..%d\n",
			        (long long)v, CURRENTMAXMAX);
			return -1;
		}
		out->current_max = (uint16_t)v;
	}
	out->peak_current = 0;
	if (read_int(ch, "peak_current", &v) == 0) {
		if (v < 0 || v > PEAKCURRENTMAX) {
			fprintf(stderr, "output-6ch: peak_current %lld out of range 0..%d\n",
			        (long long)v, PEAKCURRENTMAX);
			return -1;
		}
		out->peak_current = (uint16_t)v;
	}
	out->peak_time = 0;
	if (read_int(ch, "peak_time", &v) == 0) {
		if (v < 0 || v > UINT16_MAX) {
			fprintf(stderr, "output-6ch: peak_time %lld out of range\n", (long long)v);
			return -1;
		}
		out->peak_time = (uint16_t)v;
	}

	const char *s;
	out->name[0] = '\0';
	if (read_str(ch, "name", &s) == 0) {
		size_t n = strlen(s);
		if (n >= sizeof(out->name)) {
			fprintf(stderr, "output-6ch: name \"%s\" too long\n", s);
			return -1;
		}
		memcpy(out->name, s, n + 1);
	}
	return 0;
}

static int parse_channels(struct json_object *arr, struct ch_cfg ch[NUM_CHANNELS])
{
	for (int i = 0; i < NUM_CHANNELS; i++) {
		ch[i].func = OUTPUTFUNC_DISABLED;
		ch[i].current_max = ch[i].peak_current = ch[i].peak_time = 0;
		ch[i].cmd_fd = ch[i].current_fd = ch[i].duty_fd = -1;
		ch[i].name[0] = '\0';
	}
	if (!arr) return 0;

	size_t n = json_object_array_length(arr);
	for (size_t i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(arr, i);
		int64_t cnum;
		if (read_int(e, "channel", &cnum) != 0 || cnum < 1 || cnum > NUM_CHANNELS) {
			fprintf(stderr, "output-6ch: bad channel index in entry %zu\n", i);
			return -1;
		}
		if (parse_channel(e, &ch[cnum - 1]) != 0) return -1;
	}
	return 0;
}

#define CHECK(call) do { \
	int _rc = (call); \
	if (_rc != 0) { \
		fprintf(stderr, "output-6ch: %s -> %d\n", #call, _rc); \
		return _rc; \
	} \
} while (0)

static void close_fds(struct state *st)
{
	for (int c = 0; c < NUM_CHANNELS; c++) {
		if (st->ch[c].cmd_fd     >= 0) close(st->ch[c].cmd_fd);
		if (st->ch[c].current_fd >= 0) close(st->ch[c].current_fd);
		if (st->ch[c].duty_fd    >= 0) close(st->ch[c].duty_fd);
	}
	if (st->fd_temperature   >= 0) close(st->fd_temperature);
	if (st->fd_ground        >= 0) close(st->fd_ground);
	if (st->fd_supply        >= 0) close(st->fd_supply);
	if (st->fd_error_code    >= 0) close(st->fd_error_code);
}

static int output_6ch_init(struct driver *drv, const struct slot_cfg *cfg)
{
	struct state *st = calloc(1, sizeof(*st));
	if (!st) {
		fprintf(stderr, "output-6ch: calloc: %s\n", strerror(errno));
		return -1;
	}
	st->fd_temperature = st->fd_ground = st->fd_supply =
		st->fd_error_code = -1;
	for (int c = 0; c < NUM_CHANNELS; c++) {
		st->ch[c].cmd_fd = st->ch[c].current_fd = st->ch[c].duty_fd = -1;
	}
	st->slot = cfg->slot;
	drv->state = st;

	if (parse_freq_pairs(cfg->module_obj, st->freq) != 0) goto fail;
	if (parse_channels(cfg->channels_obj, st->ch) != 0) goto fail;

	uint8_t slot0 = (uint8_t)(cfg->slot - 1);

	CHECK(GO_module_output_set_module_type(&st->module, OUTPUTMODULE6CHANNEL));

	if (GO_communication_modules_initialize(slot0) != 0) {
		fprintf(stderr, "output-6ch: GO_communication_modules_initialize(%u) failed\n", slot0);
		goto fail;
	}
	CHECK(GO_module_output_set_module_slot(&st->module, slot0));

	for (uint8_t p = 0; p < NUM_FREQ_PAIRS; p++)
		CHECK(GO_module_output_configure_frequency(&st->module, p, st->freq[p]));

	for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
		CHECK(GO_module_output_6ch_configure_channel(&st->module, c,
		      st->ch[c].func, st->ch[c].current_max,
		      st->ch[c].peak_current, st->ch[c].peak_time));
	}
	CHECK(GO_module_output_configuration(&st->module));

	for (int c = 0; c < NUM_CHANNELS; c++) {
		st->ch[c].cmd_fd     = shm_open_attr(cfg->slot, c, "value",   O_RDWR | O_CREAT);
		st->ch[c].current_fd = shm_open_attr(cfg->slot, c, "current", O_WRONLY | O_CREAT | O_TRUNC);
		st->ch[c].duty_fd    = shm_open_attr(cfg->slot, c, "duty",    O_WRONLY | O_CREAT | O_TRUNC);
		if (st->ch[c].cmd_fd < 0 || st->ch[c].current_fd < 0 || st->ch[c].duty_fd < 0)
			goto fail;
		if (st->ch[c].name[0])
			(void)shm_alias_create(st->ch[c].name, cfg->slot, c);
	}

	st->fd_temperature = shm_open_attr(cfg->slot, -1, "temperature", O_WRONLY | O_CREAT | O_TRUNC);
	st->fd_ground      = shm_open_attr(cfg->slot, -1, "ground",      O_WRONLY | O_CREAT | O_TRUNC);
	st->fd_supply      = shm_open_attr(cfg->slot, -1, "supply",      O_WRONLY | O_CREAT | O_TRUNC);
	st->fd_error_code  = shm_open_attr(cfg->slot, -1, "error_code",  O_WRONLY | O_CREAT | O_TRUNC);
	if (st->fd_temperature < 0 || st->fd_ground < 0 || st->fd_supply < 0 ||
	    st->fd_error_code < 0) goto fail;

	fprintf(stderr, "output-6ch: slot %d ready\n", cfg->slot);
	return 0;

fail:
	close_fds(st);
	free(st);
	drv->state = NULL;
	return -1;
}

static int output_6ch_tick(struct driver *drv)
{
	struct state *st = drv->state;
	st->tick_count++;

	for (int c = 0; c < NUM_CHANNELS; c++) {
		int32_t v = 0;
		if (shm_read_int(st->ch[c].cmd_fd, &v) == 0) {
			if (v < 0) v = 0;
			if (v > UINT16_MAX) v = UINT16_MAX;
			st->module.value[c] = (uint16_t)v;
		}
	}

	int rc = GO_module_output_send_values(&st->module);
	if (rc != 0 && (st->err_count++ % 1000) == 0)
		fprintf(stderr, "output-6ch: slot %d send_values rc=%d errorCode=0x%08x (err=%llu)\n",
		        st->slot, rc, st->module.errorCode,
		        (unsigned long long)st->err_count);

	/* Always publish feedback: on SPI failure, errorCode reflects the failure
	   mode (0x10000000 = generic, 0x20000000 = not registered) so host apps see
	   the live state. */
	for (int c = 0; c < NUM_CHANNELS; c++) {
		shm_write_int(st->ch[c].current_fd, (int32_t)st->module.current[c]);
		shm_write_int(st->ch[c].duty_fd,    (int32_t)st->module.dutyCycle[c]);
	}
	shm_write_int(st->fd_temperature, (int32_t)st->module.temperature);
	shm_write_int(st->fd_ground,      (int32_t)st->module.ground);
	shm_write_int(st->fd_supply,      (int32_t)st->module.supply);
	shm_write_int(st->fd_error_code,  (int32_t)st->module.errorCode);
	return rc;
}

static void output_6ch_shutdown(struct driver *drv)
{
	struct state *st = drv->state;
	if (!st) return;
	close_fds(st);
	free(st);
	drv->state = NULL;
}

const struct driver_ops output_6ch_ops = {
	.type     = "output-6ch",
	.init     = output_6ch_init,
	.tick     = output_6ch_tick,
	.shutdown = output_6ch_shutdown,
};
