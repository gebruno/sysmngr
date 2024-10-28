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

#include "utils.h"

/*************************************************************
* ENTRY METHOD
**************************************************************/
static int browseDeviceInfoRebootsRebootInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct dm_data *p = NULL;
	char *inst = NULL;
	LIST_HEAD(dup_list);

	synchronize_specific_config_sections_with_dmmap("deviceinfo", "reboot", "dmmap_deviceinfo", &dup_list);
	list_for_each_entry(p, &dup_list, list) {

		inst = handle_instance(dmctx, parent_node, p->dmmap_section, "reboot_instance", "reboot_alias");

		if (DM_LINK_INST_OBJ(dmctx, parent_node, (void *)p, inst) == DM_STOP)
			break;
	}
	free_dmmap_config_dup_list(&dup_list);
	return 0;
}

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_DeviceInfoReboots_BootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("deviceinfo", "globals", "boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_CurrentVersionBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("deviceinfo", "globals", "curr_version_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_WatchdogBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("deviceinfo", "globals", "watchdog_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_ColdBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("deviceinfo", "globals", "cold_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_WarmBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("deviceinfo", "globals", "warm_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_MaxRebootEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("deviceinfo", "globals", "max_reboot_entries", "3");
	return 0;
}

static int set_DeviceInfoReboots_MaxRebootEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	struct uci_section *s = NULL, *tmp_s = NULL;
	int max_entries = DM_STRTOL(value);

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_int(ctx, value, RANGE_ARGS{{"-1",NULL}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			if (max_entries == 0) {
				// Delete all sections if value is "0"
				uci_foreach_sections_safe("deviceinfo", "reboot", tmp_s, s) {
					dmuci_delete_by_section(s, NULL, NULL);
				}
			} else if (max_entries > 0) {
				// Step 1: Count total sections
				int total_sections = 0;

				uci_foreach_sections_safe("deviceinfo", "reboot", tmp_s, s) {
					total_sections++;
				}

				// Step 2: Calculate how many sections to delete (earliest sections)
				int to_delete = total_sections - max_entries;

				// Step 3: Delete the earliest sections that exceed max_entries
				if (to_delete > 0) {
					int idx = 0;
					uci_foreach_sections_safe("deviceinfo", "reboot", tmp_s, s) {
						if (idx++ < to_delete) {
							dmuci_delete_by_section(s, NULL, NULL);
						}
					}
				}
			}

			dmuci_set_value("deviceinfo", "globals", "max_reboot_entries", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoReboots_RebootNumberOfEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseDeviceInfoRebootsRebootInst);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_DeviceInfoRebootsReboot_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	return bbf_get_alias(ctx, ((struct dm_data *)data)->dmmap_section, "reboot_alias", instance, value);
}

static int set_DeviceInfoRebootsReboot_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	return bbf_set_alias(ctx, ((struct dm_data *)data)->dmmap_section, "reboot_alias", instance, value);
}

static int get_DeviceInfoRebootsReboot_TimeStamp(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "time_stamp", value);
	return 0;
}

static int get_DeviceInfoRebootsReboot_FirmwareUpdated(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "firmware_updated", value);
	return 0;
}

static int get_DeviceInfoRebootsReboot_Cause(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "cause", value);
	return 0;
}

static int get_DeviceInfoRebootsReboot_Reason(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "reason", value);
	return 0;
}

/*************************************************************
 * OPERATE COMMANDS
 *************************************************************/
static int operate_DeviceInfoReboots_RemoveAllReboots(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	struct uci_section *s = NULL, *tmp_s = NULL;

	uci_foreach_sections_safe("deviceinfo", "reboot", tmp_s, s) {
		dmuci_delete_by_section(s, NULL, NULL);
	}
    return 0;
}

static int operate_DeviceInfoRebootsReboot_Remove(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	dmuci_delete_by_section(((struct dm_data *)data)->config_section, NULL, NULL);
    return 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.Reboots.Reboot.{i}. *** */
DMLEAF tDeviceInfoRebootsRebootParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"Alias", &DMWRITE, DMT_STRING, get_DeviceInfoRebootsReboot_Alias, set_DeviceInfoRebootsReboot_Alias, BBFDM_USP},
{"TimeStamp", &DMREAD, DMT_TIME, get_DeviceInfoRebootsReboot_TimeStamp, NULL, BBFDM_USP},
{"FirmwareUpdated", &DMREAD, DMT_BOOL, get_DeviceInfoRebootsReboot_FirmwareUpdated, NULL, BBFDM_USP},
{"Cause", &DMREAD, DMT_STRING, get_DeviceInfoRebootsReboot_Cause, NULL, BBFDM_USP},
{"Reason", &DMREAD, DMT_STRING, get_DeviceInfoRebootsReboot_Reason, NULL, BBFDM_USP},
{"Remove()", &DMASYNC, DMT_COMMAND, NULL, operate_DeviceInfoRebootsReboot_Remove, BBFDM_USP},
{0}
};

/* *** Device.DeviceInfo.Reboots. *** */
DMOBJ tDeviceInfoRebootsObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type, uniqueKeys */
{"Reboot", &DMREAD, NULL, NULL, NULL, browseDeviceInfoRebootsRebootInst, NULL, NULL, NULL, tDeviceInfoRebootsRebootParams, NULL, BBFDM_USP, NULL},
{0}
};

DMLEAF tDeviceInfoRebootsParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"BootCount", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_BootCount, NULL, BBFDM_USP},
{"CurrentVersionBootCount", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_CurrentVersionBootCount, NULL, BBFDM_USP},
{"WatchdogBootCount", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_WatchdogBootCount, NULL, BBFDM_USP},
{"ColdBootCount", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_ColdBootCount, NULL, BBFDM_USP},
{"WarmBootCount", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_WarmBootCount, NULL, BBFDM_USP},
{"MaxRebootEntries", &DMWRITE, DMT_INT, get_DeviceInfoReboots_MaxRebootEntries, set_DeviceInfoReboots_MaxRebootEntries, BBFDM_USP},
{"RebootNumberOfEntries", &DMREAD, DMT_UNINT, get_DeviceInfoReboots_RebootNumberOfEntries, NULL, BBFDM_USP},
{"RemoveAllReboots()", &DMASYNC, DMT_COMMAND, NULL, operate_DeviceInfoReboots_RemoveAllReboots, BBFDM_USP},
{0}
};
