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

#ifdef SYSMNGR_PROCESS_STATUS
#include "processes.h"
#endif

#ifdef SYSMNGR_MEMORY_STATUS
#include "memory.h"
#endif

#if defined(SYSMNGR_FWBANK_UBUS_SUPPORT) || defined(SYSMNGR_FIRMWARE_IMAGE)
#include "fwbank.h"
#endif

#define DEFAULT_LOG_LEVEL LOG_INFO

extern DM_MAP_OBJ tDynamicObj[];

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "    -l  <0-7> Set the loglevel\n");
	fprintf(stderr, "    -h  Displays this help\n");
	fprintf(stderr, "\n");
}

static void config_reload_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
			  const char *type, struct blob_attr *msg)
{
	BBFDM_INFO("Reloading sysmngr upon 'sysmngr.reload' event");

#ifdef SYSMNGR_PROCESS_STATUS
	sysmngr_cpu_clean();
	sysmngr_cpu_init();

	sysmngr_process_clean(ctx);
	sysmngr_process_init(ctx);
#endif

#ifdef SYSMNGR_MEMORY_STATUS
	sysmngr_memory_clean();
	sysmngr_memory_init();
#endif

}

int main(int argc, char **argv)
{
	struct bbfdm_context bbfdm_ctx = {0};
	struct ubus_event_handler ev = {
		.cb = config_reload_cb,
	};
	int log_level = DEFAULT_LOG_LEVEL;
	int c = 0;

	while ((c = getopt(argc, argv, "hl:")) != -1) {
		switch (c) {
		case 'l':
			log_level = (int)strtod(optarg, NULL);
			if (log_level < 0 || log_level > 7) {
				log_level = DEFAULT_LOG_LEVEL;
			}
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

#if defined(SYSMNGR_FWBANK_UBUS_SUPPORT) || defined(SYSMNGR_FIRMWARE_IMAGE)
	sysmngr_init_fwbank_dump(&bbfdm_ctx.ubus_ctx);
#endif

#ifdef SYSMNGR_PROCESS_STATUS
	sysmngr_process_init(&bbfdm_ctx.ubus_ctx);
	sysmngr_cpu_init();
#endif

#ifdef SYSMNGR_MEMORY_STATUS
	sysmngr_memory_init();
#endif

	if (bbfdm_ubus_regiter_init(&bbfdm_ctx))
		goto out;

#ifdef SYSMNGR_FWBANK_UBUS_SUPPORT
	if (sysmngr_register_fwbank(&bbfdm_ctx.ubus_ctx))
		goto out;
#endif

	if (ubus_register_event_handler(&bbfdm_ctx.ubus_ctx, &ev, "sysmngr.reload"))
		goto out;

	uloop_run();

out:
	bbfdm_ubus_regiter_free(&bbfdm_ctx);

#ifdef SYSMNGR_PROCESS_STATUS
	sysmngr_process_clean(&bbfdm_ctx.ubus_ctx);
	sysmngr_cpu_clean();
#endif

#ifdef SYSMNGR_MEMORY_STATUS
	sysmngr_memory_clean();
#endif

#ifdef SYSMNGR_FWBANK_UBUS_SUPPORT
	sysmngr_unregister_fwbank(&bbfdm_ctx.ubus_ctx);
#endif

#if defined(SYSMNGR_FWBANK_UBUS_SUPPORT) || defined(SYSMNGR_MEMORY_STATUS)
	sysmngr_clean_fwbank_dump(&bbfdm_ctx.ubus_ctx);
#endif

	closelog();

	return 0;
}
