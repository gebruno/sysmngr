/*
 * Copyright (C) 2019-2024 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 */

#include "utils.h"

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_memory_status_total(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	json_object *res = NULL;
	dmubus_call("system", "info", UBUS_ARGS{{}}, 0, &res);
	DM_ASSERT(res, *value = dmstrdup("0"));
	char *total = dmjson_get_value(res, 2, "memory", "total");
	dmasprintf(value, "%lu", DM_STRTOUL(total) / 1024);
	return 0;
}

static int get_memory_status_free(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	json_object *res = NULL;
	dmubus_call("system", "info", UBUS_ARGS{{}}, 0, &res);
	DM_ASSERT(res, *value = dmstrdup("0"));
	char *free = dmjson_get_value(res, 2, "memory", "free");
	dmasprintf(value, "%lu", DM_STRTOUL(free) / 1024);
	return 0;
}

static int get_memory_status_total_persistent(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct statvfs dinfo;

	if (statvfs("/overlay/", &dinfo) == 0) {
		unsigned int total = (dinfo.f_bsize * dinfo.f_blocks) / 1024;
		dmasprintf(value, "%u", total);
	} else {
		*value = dmstrdup("0");
	}

	return 0;
}

static int get_memory_status_free_persistent(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct statvfs dinfo;

	if (statvfs("/overlay/", &dinfo) == 0) {
		unsigned int free = (dinfo.f_bsize * dinfo.f_bavail) / 1024;
		dmasprintf(value, "%u", free);
	} else {
		*value = dmstrdup("0");
	}

	return 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.MemoryStatus. *** */
DMLEAF tDeviceInfoMemoryStatusParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"Total", &DMREAD, DMT_UNINT, get_memory_status_total, NULL, BBFDM_BOTH},
{"Free", &DMREAD, DMT_UNINT, get_memory_status_free, NULL, BBFDM_BOTH},
{"TotalPersistent", &DMREAD, DMT_UNINT, get_memory_status_total_persistent, NULL, BBFDM_BOTH},
{"FreePersistent", &DMREAD, DMT_UNINT, get_memory_status_free_persistent, NULL, BBFDM_BOTH},
{0}
};
