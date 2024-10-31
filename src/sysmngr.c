/*
 * Copyright (C) 2024 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/uloop.h>
#include <libbbfdm-ubus/bbfdm-ubus.h>

#ifdef SYSMNGR_REBOOTS
#include "reboots.h"
#endif

extern DM_MAP_OBJ tDynamicObj[];

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "    -d  Use multiple time to get more verbose debug logs (Debug: -dddd)\n");
	fprintf(stderr, "    -h  Displays this help\n");
	fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
	struct bbfdm_context bbfdm_ctx = {0};
	int log_level = LOG_ERR;
	int c = 0;

	while ((c = getopt(argc, argv, "dh")) != -1) {
		switch (c) {
		case 'd':
			log_level += 1;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	memset(&bbfdm_ctx, 0, sizeof(struct bbfdm_context));

	bbfdm_ubus_set_service_name(&bbfdm_ctx, "sysmngr");
	bbfdm_ubus_set_log_level(log_level);
	bbfdm_ubus_load_data_model(tDynamicObj);

	openlog("sysmngr", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

#ifdef SYSMNGR_REBOOTS
	sysmngr_reboots_init();
#endif

	if (bbfdm_ubus_regiter_init(&bbfdm_ctx))
		goto out;

	uloop_run();

out:
	bbfdm_ubus_regiter_free(&bbfdm_ctx);
	closelog();

	return 0;
}
