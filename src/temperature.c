/*
 * Copyright (C) 2024 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Suvendhu Hansa <suvendhu.hansa@iopsys.eu>
 *
 */

#include "libbbfdm-api/dmcommon.h"

#define TEMPERATURE_SCRIPT "/etc/sysmngr/temperature.sh"
#define TEMPERATURE_STATUS_CMD TEMPERATURE_SCRIPT " status"

static void get_temperature_status(json_object **temp_status)
{
	char res[1024] = {0};

	if (temp_status == NULL)
		return;

	*temp_status = NULL;
	if (run_cmd(TEMPERATURE_STATUS_CMD, res, sizeof(res)) != 0)
		return;

	if (DM_STRLEN(res) != 0) {
		remove_new_line(res);
		*temp_status = json_tokener_parse(res);
	}
}

static int browseTemperatureSensor(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	json_object *temp_status = NULL;
	struct dm_data data = {0};

	get_temperature_status(&temp_status);
	if (temp_status) { /* cppcheck-suppress knownConditionTrueFalse */
		int id = 0, iter = 0;
		json_object *arrobj = NULL, *tobj = NULL;

		dmjson_foreach_obj_in_array(temp_status, arrobj, tobj, iter, 1, "status") {
			char *inst;

			data.json_object = tobj;

			inst = handle_instance_without_section(dmctx, parent_node, ++id);
			if (DM_LINK_INST_OBJ(dmctx, parent_node, (void *)&data, inst) == DM_STOP)
				break;
		}

		json_object_put(temp_status);
	}

	return 0;
}

static int get_TemperatureStatus_numentries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseTemperatureSensor);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_TemperatureSensor_alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct uci_section *s = NULL;

	uci_path_foreach_option_eq(bbfdm, "dmmap", "temperature", "temperature_instance", instance, s) {
		dmuci_get_value_by_section_string(s, "alias", value);
		break;
	}
	if ((*value)[0] == '\0')
		dmasprintf(value, "cpe-%s", instance);
	return 0;
}

static int set_TemperatureSensor_alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	struct uci_section *s = NULL, *dmmap = NULL;
	switch (action) {
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, value, -1, 64, NULL, NULL))
				return FAULT_9007;
			break;
		case VALUESET:
			uci_path_foreach_option_eq(bbfdm, "dmmap", "temperature", "temperature_instance", instance, s) {
				dmuci_set_value_by_section_bbfdm(s, "alias", value);
				return 0;
			}
			dmuci_add_section_bbfdm("dmmap", "temperature", &dmmap);
			dmuci_set_value_by_section(dmmap, "temperature_instance", instance);
			dmuci_set_value_by_section(dmmap, "alias", value);
			break;
	}
	return 0;
}

static int get_TemperatureSensor_name(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	json_object *obj = ((struct dm_data *)data)->json_object;
	*value = dmjson_get_value(obj, 1, "name");
	return 0;
}

static int get_TemperatureSensor_value(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	json_object *obj = ((struct dm_data *)data)->json_object;
	*value = dmjson_get_value(obj, 1, "temperature");
	return 0;
}

/******************************************************************************************************************************
*                                            OBJ & PARAM DEFINITION
*******************************************************************************************************************************/
static DMLEAF tTemperatureStatusTemperatureSensorParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type*/
{"Alias", &DMWRITE, DMT_STRING, get_TemperatureSensor_alias, set_TemperatureSensor_alias, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Name", &DMREAD, DMT_STRING, get_TemperatureSensor_name, NULL, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Value", &DMREAD, DMT_INT, get_TemperatureSensor_value, NULL, BBFDM_BOTH},
{0}
};

/* *** Device.DeviceInfo.TemperatureTemperatureStatus; ubus call "bbf.temp status" *** */
DMOBJ tDeviceInfoTemperatureStatusObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type, uniqueKeys*/
{"TemperatureSensor", &DMREAD, NULL, NULL, NULL, browseTemperatureSensor, NULL, NULL, NULL, tTemperatureStatusTemperatureSensorParams, NULL, BBFDM_BOTH},
{0}
};

DMLEAF tDeviceInfoTemperatureStatusParams[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type, uniqueKeys*/
{"TemperatureSensorNumberOfEntries", &DMREAD, DMT_UNINT, get_TemperatureStatus_numentries, NULL, BBFDM_BOTH},
{0}
};
