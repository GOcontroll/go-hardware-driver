#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GO_board.h"

#include "config.h"
#include "driver.h"
#include "registry.h"
#include "shm.h"

#define CONFIG_PATH    "/usr/lib/firmware/gocontroll/modules.json"
#define LOOP_PERIOD_NS 10000000L     /* 100 Hz */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void install_signals(void)
{
	struct sigaction sa = { 0 };
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void timespec_add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec  += 1;
	}
}

int main(void)
{
	install_signals();

	if (shm_base_init() != 0) return EXIT_FAILURE;

	/* Once for the whole process: detect controller hardware and zero the
	   global moduleOccupancy table. Calling this per driver would wipe
	   registrations of previously-initialized slots. */
	GO_board_get_hardware_version();

	struct config_file cf;
	if (config_load(CONFIG_PATH, &cf) != 0) return EXIT_FAILURE;

	fprintf(stderr, "main: schema %s controller %s, %zu slot(s)\n",
	        cf.schema_version, cf.controller, cf.n_slots);

	struct driver *drivers = calloc(cf.n_slots, sizeof(*drivers));
	if (!drivers) {
		fprintf(stderr, "main: calloc: %s\n", strerror(errno));
		config_free(&cf);
		return EXIT_FAILURE;
	}
	size_t n_drv = 0;

	for (size_t i = 0; i < cf.n_slots; i++) {
		const struct slot_cfg *sc = &cf.slots[i];
		const struct driver_ops *ops = registry_lookup(sc->module_type);
		if (!ops) {
			fprintf(stderr, "main: slot %d type \"%s\" — no driver, skipped\n",
			        sc->slot, sc->module_type);
			continue;
		}
		drivers[n_drv].ops = ops;
		drivers[n_drv].cfg = sc;
		if (ops->init(&drivers[n_drv], sc) != 0) {
			fprintf(stderr, "main: slot %d type \"%s\" init failed\n",
			        sc->slot, sc->module_type);
			for (size_t j = 0; j < n_drv; j++) drivers[j].ops->shutdown(&drivers[j]);
			free(drivers);
			config_free(&cf);
			return EXIT_FAILURE;
		}
		n_drv++;
	}

	if (n_drv == 0) {
		fprintf(stderr, "main: no drivers configured, exiting\n");
		free(drivers);
		config_free(&cf);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "main: %zu driver(s) running at 100 Hz\n", n_drv);

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (!g_stop) {
		for (size_t i = 0; i < n_drv; i++)
			(void)drivers[i].ops->tick(&drivers[i]);

		timespec_add_ns(&next, LOOP_PERIOD_NS);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && !g_stop)
			;
	}

	fprintf(stderr, "main: shutting down\n");
	for (size_t i = 0; i < n_drv; i++) drivers[i].ops->shutdown(&drivers[i]);
	free(drivers);
	config_free(&cf);
	return EXIT_SUCCESS;
}
