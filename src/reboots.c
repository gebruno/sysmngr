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
#include "reboots.h"

#include <uci.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REBOOT_LOCK_FILE "/tmp/bbf_reboot_handler.lock" // Lock file indicating that the boot action has already been executed
#define RESET_REASON_PATH "/tmp/reset_reason" // Path to the file containing the reason for the most recent boot/reset
#define REBOOT_MAX_RETRIES 10 // Maximum number of retries for checking if RESET_REASON_PATH has been generated
#define REBOOT_RETRY_DELAY 2 // Delay in seconds between retries when checking for the existence of RESET_REASON_PATH
#define REBOOT_MAX_ENTRIES 32 // Maximum number of reboot entries to display in Device.DeviceInfo.Reboots.Reboot.{i}

static int g_retry_count = 0;

static void reset_option_counter(const char *option_name, const char *option_value)
{
	sysmngr_uci_set("sysmngr", "reboots", option_name, option_value);
}

static void increment_option_counter(const char *option_name)
{
	char buf[16] = {0};

	sysmngr_uci_get("sysmngr", "reboots", option_name, "0", buf, sizeof(buf));

	int counter = (int)strtol(buf, NULL, 10) + 1;

	snprintf(buf, sizeof(buf), "%d", counter);
	sysmngr_uci_set("sysmngr", "reboots", option_name, buf);
}

static void get_boot_option_value(const char *option_name, char *buffer, size_t buffer_size)
{
	char line[256] = {0};

	if (!option_name || !buffer || !buffer_size)
		return;

	buffer[0] = '\0';

	FILE *file = fopen(RESET_REASON_PATH, "r");
	if (!file)
		return;

	while (fgets(line, sizeof(line), file)) {
		remove_new_line(line);

		if (strstr(line, option_name)) {
			char *p = strchr(line, ':');
			snprintf(buffer, buffer_size, "%s", p ? p + 2 : "");
			break;
		}
	}

	fclose(file);
}

static const char *boot_reason_message(const char *trigger, const char *reason)
{
	// Generate a human-readable message based on the boot reason and trigger
	if (strlen(trigger)) {
		if (strcmp(trigger, "defaultreset") == 0)
			return "FACTORY RESET";
		else if (strcmp(trigger, "upgrade") == 0)
			return "FIRMWARE UPGRADE";
		else
			return trigger;
	} else if (strlen(reason)) {
		if (strcmp(reason, "POR_RESET") == 0)
			return "POWER ON RESET";
		else
			return reason;
	} else {
		return "Unknown";
	}
}

static void calculate_boot_time(char *buffer, size_t buffer_size)
{
	// Get current time and uptime in seconds
	time_t current_time = time(NULL);
	int uptime = sysmngr_get_uptime();

	// Calculate the boot time by subtracting the uptime from the current time
	current_time -= uptime;

	struct tm *tm_info = gmtime(&current_time);

	// Convert the boot time to a human-readable format
	strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

static void delete_excess_reboot_sections(int max_reboot_entries)
{
	struct uci_ptr ptr = {0};
	char uci_str[16] = {0};

	struct uci_context *ctx = uci_alloc_context();
	if (!ctx) {
		BBF_ERR("Failed to allocate UCI context.");
		return;
	}

	snprintf(uci_str, sizeof(uci_str), "%s", "sysmngr");

	if (uci_lookup_ptr(ctx, &ptr, uci_str, true) != UCI_OK) {
		BBF_ERR("Failed to lookup for sysmngr config");
		uci_free_context(ctx);
		return;
	}

	int total_reboot_sections = 0;
	struct uci_element *e, *tmp;

	// First pass to count total reboot sections
	uci_foreach_element(&ptr.p->sections, e) {
		struct uci_section *section = uci_to_section(e);
		if (strcmp(section->type, "reboot") == 0) {
			total_reboot_sections++;
		}
	}

	// Calculate number of sections to remove
	int sections_to_remove = total_reboot_sections - ((max_reboot_entries > 0) ? max_reboot_entries : REBOOT_MAX_ENTRIES);
	if (sections_to_remove < 0) {  // No need to delete sections
		uci_free_context(ctx);
		return;
	}

	int removed_count = 0;
	// Second pass to remove excess sections
	uci_foreach_element_safe(&ptr.p->sections, tmp, e) {
		struct uci_section *section = uci_to_section(e);

		if (strcmp(section->type, "reboot") == 0 && removed_count <= sections_to_remove) {

			if (sysmngr_uci_delete(ctx, "sysmngr", section->e.name)) {
				uci_free_context(ctx);
				return;
			}

			removed_count++;
		}
	}

	// Commit changes to save deletions
	if (uci_commit(ctx, &ptr.p, false) != UCI_OK) {
		BBF_ERR("Failed to commit changes");
	}

	uci_free_context(ctx);
}

static void create_reboot_section(const char *trigger, const char *reason)
{
	char boot_time[sizeof("0001-01-01T00:00:00Z")] = {0};
	char sec_name[32] = {0};

	if (!trigger || !reason)
		return;

	snprintf(sec_name, sizeof(sec_name), "reboot_%ld", (long int)time(NULL));
	calculate_boot_time(boot_time, sizeof(boot_time));

	sysmngr_uci_set("sysmngr", sec_name, NULL, "reboot");
	sysmngr_uci_set("sysmngr", sec_name, "time_stamp", boot_time);
	sysmngr_uci_set("sysmngr", sec_name, "firmware_updated", strcmp(trigger, "upgrade") == 0 ? "1" : "0");

	if (strcmp(trigger, "defaultreset") == 0) {
		sysmngr_uci_set("sysmngr", sec_name, "cause", "FactoryReset");
	} else {
		char last_reboot_cause[32] = {0};
		sysmngr_uci_get("sysmngr", "reboots", "last_reboot_cause", "LocalReboot", last_reboot_cause, sizeof(last_reboot_cause));
		sysmngr_uci_set("sysmngr", sec_name, "cause", last_reboot_cause);
		sysmngr_uci_set("sysmngr", "reboots", "last_reboot_cause", "");
	}

	sysmngr_uci_set("sysmngr", sec_name, "reason", boot_reason_message(trigger, reason));
}

static void sysmngr_register_boot_action(void)
{
	char trigger[16] = {0}, reason[16] = {0}, max_entries[16] = {0};

	// Check if boot action was already executed
	if (file_exists(REBOOT_LOCK_FILE)) {
		BBF_INFO("Boot action already completed previously. Skipping registration.");
		return;
	}

	get_boot_option_value("triggered", trigger, sizeof(trigger));
	get_boot_option_value("reason", reason, sizeof(reason));

	if (strcmp(trigger, "defaultreset") == 0) {
		reset_option_counter("boot_count", "1");
		reset_option_counter("curr_version_boot_count", "0");
	} else {
		increment_option_counter("boot_count");
		increment_option_counter("curr_version_boot_count");
	}

	if (strstr(reason, "watchdog")) {
		increment_option_counter("watchdog_boot_count");
	}

	if (strcmp(reason, "POR_RESET") == 0) {
		increment_option_counter("cold_boot_count");
	} else {
		increment_option_counter("warm_boot_count");
	}

	sysmngr_uci_get("sysmngr", "reboots", "max_reboot_entries", "3", max_entries, sizeof(max_entries));
	int max_reboot_entries = (int)strtol(max_entries, NULL, 10);

	if (max_reboot_entries != 0) {
		delete_excess_reboot_sections(max_reboot_entries);
		create_reboot_section(trigger, reason);
	}

	// Create a lock file to mark boot action as executed
	create_empty_file(REBOOT_LOCK_FILE);
}

static void reboot_check_timer(struct uloop_timeout *timeout)
{
	sysmngr_reboots_init();
}

static struct uloop_timeout reboot_timer = { .cb = reboot_check_timer };

/*************************************************************
* EXTERNAL APIS
**************************************************************/
void sysmngr_reboots_init(void)
{
	if (file_exists(RESET_REASON_PATH)) {
		BBF_INFO("Reset reason file '%s' found. Proceeding to register boot action", RESET_REASON_PATH);
		sysmngr_register_boot_action();
		return;
	}

	if (g_retry_count < REBOOT_MAX_RETRIES) {
		g_retry_count++;
		uloop_timeout_set(&reboot_timer, REBOOT_RETRY_DELAY * 1000);

		BBF_WARNING("Attempt %d/%d: Reset reason file '%s' not found. Retrying in %d second(s)...",
		            g_retry_count, REBOOT_MAX_RETRIES, RESET_REASON_PATH, REBOOT_RETRY_DELAY);
	} else {
		BBF_ERR("Max retries reached (%d). Reset reason file '%s' not found. Proceeding with boot action registration",
		        REBOOT_MAX_RETRIES, RESET_REASON_PATH);
		sysmngr_register_boot_action();
	}
}

/*************************************************************
* ENTRY METHOD
**************************************************************/
static int browseDeviceInfoRebootsRebootInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct dm_data *p = NULL;
	char *inst = NULL;
	LIST_HEAD(dup_list);

	synchronize_specific_config_sections_with_dmmap("sysmngr", "reboot", "dmmap_sysmngr", &dup_list);
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
	dmuci_get_option_value_string("sysmngr", "reboots", "boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_CurrentVersionBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("sysmngr", "reboots", "curr_version_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_WatchdogBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("sysmngr", "reboots", "watchdog_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_ColdBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("sysmngr", "reboots", "cold_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_WarmBootCount(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("sysmngr", "reboots", "warm_boot_count", value);
	return 0;
}

static int get_DeviceInfoReboots_MaxRebootEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "reboots", "max_reboot_entries", "3");
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
				uci_foreach_sections_safe("sysmngr", "reboot", tmp_s, s) {
					dmuci_delete_by_section(s, NULL, NULL);
				}
			} else {
				// Step 1: Count total sections
				int total_sections = 0;

				uci_foreach_sections_safe("sysmngr", "reboot", tmp_s, s) {
					total_sections++;
				}

				// Step 2: Calculate how many sections to delete (earliest sections)
				int to_delete = total_sections - ((max_entries > 0) ? max_entries : REBOOT_MAX_ENTRIES);

				// Step 3: Delete the earliest sections that exceed max_entries
				if (to_delete > 0) {
					int idx = 0;
					uci_foreach_sections_safe("sysmngr", "reboot", tmp_s, s) {
						if (idx++ < to_delete) {
							dmuci_delete_by_section(s, NULL, NULL);
						}
					}
				}
			}

			dmuci_set_value("sysmngr", "reboots", "max_reboot_entries", value);
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

	uci_foreach_sections_safe("sysmngr", "reboot", tmp_s, s) {
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
