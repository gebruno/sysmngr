/*
 * Copyright (C) 2022-2024 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 */

#include "utils.h"
#include "fwbank.h"

#define MAX_TIME_WINDOW 5

struct sysupgrade_ev_data {
	const char *bank_id;
	bool status;
};

/*************************************************************
* COMMON FUNCTIONS
**************************************************************/
static char *get_blobmsg_option_value(struct blob_attr *entry, const char *option_name)
{
	struct blob_attr *tb[9] = {0};
	char *option_value = NULL;

	if (!entry)
		return "";

	blobmsg_parse(sysmngr_bank_policy, 9, tb, blobmsg_data(entry), blobmsg_len(entry));

	if (DM_STRCMP(sysmngr_bank_policy[0].name, option_name) == 0 && tb[0]) // Name
		option_value = dmstrdup(blobmsg_get_string(tb[0]));
	else if (DM_STRCMP(sysmngr_bank_policy[1].name, option_name) == 0 && tb[1]) // ID
		dmasprintf(&option_value, "%d", blobmsg_get_u32(tb[1]));
	else if (DM_STRCMP(sysmngr_bank_policy[2].name, option_name) == 0 && tb[2]) // Active
		option_value = dmstrdup(blobmsg_get_bool(tb[2]) ? "true" : "false");
	else if (DM_STRCMP(sysmngr_bank_policy[3].name, option_name) == 0 && tb[3]) // Boot
		option_value = dmstrdup(blobmsg_get_bool(tb[3]) ? "true" : "false");
	else if (DM_STRCMP(sysmngr_bank_policy[4].name, option_name) == 0 && tb[4]) // Upgrade
		option_value = dmstrdup(blobmsg_get_bool(tb[4]) ? "true" : "false");
	else if (DM_STRCMP(sysmngr_bank_policy[5].name, option_name) == 0 && tb[5]) // Firmware Version
		option_value = dmstrdup(blobmsg_get_string(tb[5]));
	else if (DM_STRCMP(sysmngr_bank_policy[6].name, option_name) == 0 && tb[6]) // Software Version
		option_value = dmstrdup(blobmsg_get_string(tb[6]));
	else if (DM_STRCMP(sysmngr_bank_policy[7].name, option_name) == 0 && tb[7]) // OMCI Software Version
		option_value = dmstrdup(blobmsg_get_string(tb[7]));
	else if (DM_STRCMP(sysmngr_bank_policy[8].name, option_name) == 0 && tb[8]) // Status
		option_value = dmstrdup(blobmsg_get_string(tb[8]));

	return option_value ? option_value : "";
}

static char *get_fwbank_option_value(void *data, const char *option_name)
{
	char *option_value = NULL;

	option_value = get_blobmsg_option_value((struct blob_attr *)((struct dm_data *)data)->additional_data, option_name);

	return option_value ? option_value : "";
}

static char *get_fwbank_bank_id(const char *option_name)
{
	char *bank_id = NULL;

	struct blob_buf *dump_bb = sysmngr_fwbank_dump();
	if (!dump_bb) //dump output is empty
		return "";

	struct blob_attr *tb[1] = {0};

	blobmsg_parse(sysmngr_dump_policy, 1, tb, blobmsg_data(dump_bb->head), blobmsg_len(dump_bb->head));

	if (!tb[0]) // bank array is not found
		return "";

	struct blob_attr *entry = NULL;
	int rem = 0;

	blobmsg_for_each_attr(entry, tb[0], rem) { // parse bank array
		 char *is_true = get_blobmsg_option_value(entry, option_name);
		 if (DM_LSTRCMP(is_true, "true") == 0) {
			bank_id = get_blobmsg_option_value(entry, "id");
			break;
		 }
	}

	return bank_id ? bank_id : "";
}

static bool fwbank_set_bootbank(char *bank_id)
{
	int res = sysmngr_fwbank_set_bootbank((uint32_t)DM_STRTOUL(bank_id), NULL);

	return !res ? true : false;
}

static bool fwbank_upgrade(const char *path, const char *auto_activate, const char *bank_id, const char *keep_settings)
{
	json_object *json_obj = NULL;
	int res = 0;

	dmubus_call_blocking("fwbank", "upgrade", UBUS_ARGS{{"path", path, String}, {"auto_activate", auto_activate, Boolean}, {"bank", bank_id, Integer}, {"keep_settings", keep_settings, Boolean}}, 4, &json_obj);

	if (json_obj) {
		char *result = dmjson_get_value(json_obj, 1, "result");
		res = (DM_LSTRCMP(result, "ok") == 0) ? true : false;
	} else {
		res = false;
	}

	if (json_obj != NULL)
		json_object_put(json_obj);

	return res;
}

static void _exec_reboot(const void *arg1, void *arg2)
{
	char config_name[16] = {0};

	snprintf(config_name, sizeof(config_name), "%s", "sysmngr");

	// Set last_reboot_cause to 'RemoteReboot' because the upcoming reboot will be initiated by USP Operate
	dmuci_set_value(config_name, "reboots", "last_reboot_cause", "RemoteReboot");
	dmuci_commit_package(config_name);

	sleep(3);
	dmubus_call_set("rpc-sys", "reboot", UBUS_ARGS{0}, 0);
	sleep(5); // Wait for reboot to happen
	BBF_ERR("Reboot call failed with rpc-sys, trying again with system");
	dmubus_call_set("system", "reboot", UBUS_ARGS{0}, 0);
	sleep(5); // Wait for reboot
	BBF_ERR("Reboot call failed!!!");

	// Set last_reboot_cause to empty because there is a problem in the system reboot
	dmuci_set_value(config_name, "reboots", "last_reboot_cause", "");
	dmuci_commit_package(config_name);
}

static void dmubus_receive_sysupgrade(struct ubus_context *ctx, struct ubus_event_handler *ev,
				const char *type, struct blob_attr *msg)
{
	struct dmubus_event_data *data;
	struct blob_attr *msg_attr;

	if (!msg || !ev)
		return;

	data = container_of(ev, struct dmubus_event_data, ev);
	if (data == NULL)
		return;

	struct sysupgrade_ev_data *ev_data = (struct sysupgrade_ev_data *)data->ev_data;
	if (ev_data == NULL)
		return;

	size_t msg_len = (size_t)blobmsg_data_len(msg);
	__blob_for_each_attr(msg_attr, blobmsg_data(msg), msg_len) {

		if (DM_STRCMP("bank_id", blobmsg_name(msg_attr)) == 0) {
			char *attr_val = (char *)blobmsg_data(msg_attr);
			if (DM_STRCMP(attr_val, ev_data->bank_id) != 0)
				return;
		}

		if (DM_STRCMP("status", blobmsg_name(msg_attr)) == 0) {
			char *attr_val = (char *)blobmsg_data(msg_attr);
			if (DM_STRCMP(attr_val, "Downloading") == 0)
				return;
			else if (DM_STRCMP(attr_val, "Available") == 0)
				ev_data->status = true;
			else
				ev_data->status = false;
		}
	}

	uloop_end();
	return;
}

static int bbf_fw_image_download(const char *url, const char *auto_activate, const char *username, const char *password,
		const char *file_size, const char *checksum_algorithm, const char *checksum,
		const char *bank_id, const char *command, const char *obj_path, const char *commandKey, const char *keep)
{
	char fw_image_path[256] = {0};
	json_object *json_obj = NULL;
	bool activate = false, valid = false;
	int res = 0;
	char fault_msg[128] = {0};
	time_t complete_time = 0;
	time_t start_time = time(NULL);

	DM_STRNCPY(fw_image_path, "/tmp/firmware-XXXXXX", sizeof(fw_image_path));

	// Check the file system size if there is sufficient space for downloading the firmware image
	if (!validate_file_system_size(file_size)) {
		res = -1;
		snprintf(fault_msg, sizeof(fault_msg), "Available memory space is lower than required for downloading");
		goto end;
	}

	res = mkstemp(fw_image_path);
	if (res == -1) {
		snprintf(fault_msg, sizeof(fault_msg), "Operation failed due to some internal failure");
		goto end;
	} else {
		close(res); // close the fd, as only filename required
		res = 0;
	}

	// Download the firmware image
	long res_code = download_file(fw_image_path, url, username, password);
	complete_time = time(NULL);

	// Check if the download operation was successful
	if (!validate_server_response_code(url, res_code)) {
		snprintf(fault_msg, sizeof(fault_msg), "Download operation is failed, fault code (%ld)", res_code);
		res = -1;
		goto end;
	}

	// Validate the CheckSum value according to its algorithm
	if (!validate_checksum_value(fw_image_path, checksum_algorithm, checksum)) {
		res = -1;
		snprintf(fault_msg, sizeof(fault_msg), "Checksum of the file is not matched with the specified value");
		goto end;
	}

	dmubus_call_blocking("system", "validate_firmware_image", UBUS_ARGS{{"path", fw_image_path, String}}, 1, &json_obj);
	if (json_obj == NULL) {
		res = -1;
		snprintf(fault_msg, sizeof(fault_msg), "Failed in validation of the file");
		goto end;
	}

	char *val = dmjson_get_value(json_obj, 1, "valid");
	string_to_bool(val, &valid);

	// Free json_obj
	json_object_put(json_obj);
	json_obj = NULL;

	if (valid == false) {
		snprintf(fault_msg, sizeof(fault_msg), "File is not a valid firmware image");
		res = -1;
		goto end;
	}

	string_to_bool(auto_activate, &activate);

	// Apply Firmware Image
	if (!fwbank_upgrade(fw_image_path, (activate) ? "1" : "0", bank_id, DM_STRLEN(keep) ? keep : "1")) {
		res = 1;
		snprintf(fault_msg, sizeof(fault_msg), "Internal error occurred when applying the firmware");
		goto end;
	}

	struct sysupgrade_ev_data ev_data = {
		.bank_id = bank_id,
		.status = false,
	};

	dmubus_wait_for_event("sysupgrade", 120, &ev_data, dmubus_receive_sysupgrade, NULL);

	if (ev_data.status == false) {
		res = 1;
		snprintf(fault_msg, sizeof(fault_msg), "Failed to apply the downloaded image file");
		goto end;
	}

	// Schedule a device Reboot, if auto activation is true
	if (activate) {
		bbfdm_task_fork(_exec_reboot, NULL, NULL, NULL);
	}

end:
	// Send the transfer complete event
	send_transfer_complete_event(command, obj_path, url, fault_msg, start_time, complete_time, commandKey, "Download");

	// Remove temporary file if ubus upgrade failed and file exists
	if (file_exists(fw_image_path) && strncmp(url, FILE_URI, strlen(FILE_URI)))
		remove(fw_image_path);

	return res;
}

/*************************************************************
* ENTRY METHOD
**************************************************************/
int browseDeviceInfoFirmwareImageInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct dm_data curr_data = {0};
	struct blob_attr *tb[1] = {0};
	char *inst = NULL;
	int id = 0;

	struct blob_buf *dump_bb = sysmngr_fwbank_dump();
	if (!dump_bb) //dump output is empty
		return 0;

	blobmsg_parse(sysmngr_dump_policy, 1, tb, blobmsg_data(dump_bb->head), blobmsg_len(dump_bb->head));

	if (tb[0]) { // bank array defined
		struct blob_attr *entry = NULL;
		int rem = 0;

		blobmsg_for_each_attr(entry, tb[0], rem) { // parse bank array

			curr_data.additional_data = (void *)entry;

			inst = handle_instance_without_section(dmctx, parent_node, ++id);

			if (DM_LINK_INST_OBJ(dmctx, parent_node, (void *)&curr_data, inst) == DM_STOP)
				break;
		}
	}

	return 0;
}

/*************************************************************
* GET & SET PARAM
**************************************************************/
int get_device_active_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *bank_id = get_fwbank_bank_id("active");
	if (DM_STRLEN(bank_id) == 0) {
		*value = dmstrdup("");
		return 0;
	}

	char linker[16] = {0};

	snprintf(linker, sizeof(linker), "cpe-%s", bank_id);
	_bbfdm_get_references(ctx, "Device.DeviceInfo.FirmwareImage.", "Alias", linker, value);
	return 0;
}

int get_device_boot_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *bank_id = get_fwbank_bank_id("boot");
	if (DM_STRLEN(bank_id) == 0) {
		*value = dmstrdup("");
		return 0;
	}

	char linker[16] = {0};

	snprintf(linker, sizeof(linker), "cpe-%s", bank_id);
	_bbfdm_get_references(ctx, "Device.DeviceInfo.FirmwareImage.", "Alias", linker, value);
	return 0;
}

int set_device_boot_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	char *allowed_objects[] = {"Device.DeviceInfo.FirmwareImage.", NULL};
	struct dm_reference reference = {0};

	bbfdm_get_reference_linker(ctx, value, &reference);

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, reference.path, -1, -1, NULL, NULL))
				return FAULT_9007;

			if (dm_validate_allowed_objects(ctx, &reference, allowed_objects))
				return FAULT_9007;

			break;
		case VALUESET:
			if (DM_STRLEN(reference.value)) {
				struct uci_section *dmmap_s = NULL;
				char *available = NULL;

				char *bank_id = DM_STRCHR(reference.value, '-'); // Get bank id 'X' which is linker from Alias prefix 'cpe-X'
				if (!bank_id)
					return FAULT_9001;

				get_dmmap_section_of_config_section_cont("dmmap_fw_image", "fw_image", "id", bank_id + 1, &dmmap_s);
				dmuci_get_value_by_section_string(dmmap_s, "available", &available);
				if (DM_LSTRCMP(available, "false") == 0)
					return FAULT_9001;

				if (!fwbank_set_bootbank(bank_id + 1))
					return FAULT_9001;
			}
			break;
	}
	return 0;
}

int get_DeviceInfo_MaxNumberOfActivateTimeWindows(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%d", MAX_TIME_WINDOW);
	return 0;
}

int get_DeviceInfo_FirmwareImageNumberOfEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseDeviceInfoFirmwareImageInst);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_DeviceInfoFirmwareImage_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *id = get_fwbank_option_value(data, "id");
	dmasprintf(value, "cpe-%s", id ? id : instance);
	return 0;
}

static int set_DeviceInfoFirmwareImage_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, value, -1, 64, NULL, NULL))
				return FAULT_9007;
			break;
		case VALUESET:
			bbfdm_set_fault_message(ctx, "Internal designated unique identifier, not allowed to update");
			return FAULT_9007;
	}
	return 0;
}

static int get_DeviceInfoFirmwareImage_Name(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *name = get_fwbank_option_value(data, "fwver");
	if (DM_STRLEN(name) > 64 ) {
		name[64] = '\0';
	}

	*value = name;
	return 0;
}

static int get_DeviceInfoFirmwareImage_Version(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = get_fwbank_option_value(data, "swver");
	return 0;
}

static int get_DeviceInfoFirmwareImage_Available(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct uci_section *s = NULL;

	char *id = get_fwbank_option_value(data, "id");

	uci_path_foreach_option_eq(bbfdm, "dmmap_fw_image", "fw_image", "id", id, s) {
		dmuci_get_value_by_section_string(s, "available", value);
		break;
	}

	if ((*value)[0] == '\0')
		*value = dmstrdup("true");
	return 0;
}

static int set_DeviceInfoFirmwareImage_Available(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	struct uci_section *s = NULL, *dmmap = NULL;
	char *id = NULL;
	bool b;

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_boolean(ctx, value))
				return FAULT_9007;
			break;
		case VALUESET:
			string_to_bool(value, &b);

			if (!b) {
				char *boot = get_fwbank_option_value(data, "boot");
				char *active = get_fwbank_option_value(data, "active");
				if (DM_LSTRCMP(boot, "true") == 0 || DM_LSTRCMP(active, "true") == 0)
					return FAULT_9001;
			}

			id = get_fwbank_option_value(data, "id");

			uci_path_foreach_option_eq(bbfdm, "dmmap_fw_image", "fw_image", "id", id, s) {
				dmuci_set_value_by_section_bbfdm(s, "available", b ? "true" : "false");
				return 0;
			}

			dmuci_add_section_bbfdm("dmmap_fw_image", "fw_image", &dmmap);
			dmuci_set_value_by_section(dmmap, "id", id);
			dmuci_set_value_by_section(dmmap, "available", b ? "true" : "false");
			break;
	}
	return 0;
}

static int get_DeviceInfoFirmwareImage_Status(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = get_fwbank_option_value(data, "status");
	return 0;
}

/*************************************************************
 * OPERATE COMMANDS
 *************************************************************/
static operation_args firmware_image_download_args = {
	.in = (const char *[]) {
		"URL",
		"AutoActivate",
		"Username",
		"Password",
		"FileSize",
		"CheckSumAlgorithm",
		"CheckSum",
		"CommandKey",
#ifdef SYSMNGR_VENDOR_EXTENSIONS
		CUSTOM_PREFIX"KeepConfig",
#endif
		NULL
	}
};

static int get_operate_args_DeviceInfoFirmwareImage_Download(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&firmware_image_download_args;
	return 0;
}

static int operate_DeviceInfoFirmwareImage_Download(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	const char *command = "Download()";
	char obj_path[256] = {0};

	char *ret = DM_STRRCHR(refparam, '.');
	if (!ret)
		return USP_FAULT_INVALID_ARGUMENT;

	if ((ret - refparam + 2) < sizeof(obj_path))
		snprintf(obj_path, ret - refparam + 2, "%s", refparam);

	char *url = dmjson_get_value((json_object *)value, 1, "URL");
	if (url[0] == '\0')
		return USP_FAULT_INVALID_ARGUMENT;

	// Assuming auto activate as false, if not provided by controller, in case of strict validation,
	// this should result into a fault
	char *auto_activate = dmjson_get_value((json_object *)value, 1, "AutoActivate");
	if (DM_STRLEN(auto_activate) == 0)
		auto_activate = dmstrdup("0");

	char *username = dmjson_get_value((json_object *)value, 1, "Username");
	char *password = dmjson_get_value((json_object *)value, 1, "Password");
	char *file_size = dmjson_get_value((json_object *)value, 1, "FileSize");
	char *checksum_algorithm = dmjson_get_value((json_object *)value, 1, "CheckSumAlgorithm");
	char *checksum = dmjson_get_value((json_object *)value, 1, "CheckSum");
	char *commandKey = dmjson_get_value((json_object *)value, 1, "CommandKey");
	char *keep_config = NULL;
#ifdef SYSMNGR_VENDOR_EXTENSIONS
	keep_config = dmjson_get_value((json_object *)value, 1, CUSTOM_PREFIX"KeepConfig");
#endif
	char *bank_id = get_fwbank_option_value(data, "id");

	int res = bbf_fw_image_download(url, auto_activate, username, password, file_size, checksum_algorithm, checksum, bank_id, command, obj_path, commandKey, keep_config);

	if (res == 1) {
		bbfdm_set_fault_message(ctx, "Firmware validation failed");
	}

	return res ? USP_FAULT_COMMAND_FAILURE : 0;
}

static operation_args firmware_image_activate_args = {
	.in = (const char *[]) {

		"TimeWindow.{i}.Start",
		"TimeWindow.{i}.End",
		"TimeWindow.{i}.Mode",
		"TimeWindow.{i}.UserMessage",
		"TimeWindow.{i}.MaxRetries",
		NULL
	}
};

static int get_operate_args_DeviceInfoFirmwareImage_Activate(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&firmware_image_activate_args;
	return 0;
}

static int operate_DeviceInfoFirmwareImage_Activate(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
#define CRONTABS_ROOT "/etc/crontabs/root"
#define ACTIVATE_HANDLER_FILE "/usr/share/bbfdm/scripts/bbf_activate_handler.sh"

	char *FW_Mode[] = {"AnyTime", "Immediately", "WhenIdle", "ConfirmationNeeded", NULL};
	char *start_time[MAX_TIME_WINDOW] = {0};
	char *end_time[MAX_TIME_WINDOW] = {0};
	char *mode[MAX_TIME_WINDOW] = {0};
	char *user_message[MAX_TIME_WINDOW] = {0};
	char *max_retries[MAX_TIME_WINDOW] = {0};
	int res = 0, last_idx = -1;

	for (int i = 0; i < MAX_TIME_WINDOW; i++) {
		char buf[32] = {0};

		snprintf(buf, sizeof(buf), "TimeWindow.%d.Start", i + 1);
		start_time[i] = dmjson_get_value((json_object *)value, 1, buf);

		snprintf(buf, sizeof(buf), "TimeWindow.%d.End", i + 1);
		end_time[i] = dmjson_get_value((json_object *)value, 1, buf);

		snprintf(buf, sizeof(buf), "TimeWindow.%d.Mode", i + 1);
		mode[i] = dmjson_get_value((json_object *)value, 1, buf);

		snprintf(buf, sizeof(buf), "TimeWindow.%d.UserMessage", i + 1);
		user_message[i] = dmjson_get_value((json_object *)value, 1, buf);

		snprintf(buf, sizeof(buf), "TimeWindow.%d.MaxRetries", i + 1);
		max_retries[i] = dmjson_get_value((json_object *)value, 1, buf);

		if (!DM_STRLEN(start_time[i]))
			break;

		if (!DM_STRLEN(end_time[i]) || !DM_STRLEN(mode[i]))
			return USP_FAULT_INVALID_ARGUMENT;

		if (bbfdm_validate_unsignedInt(ctx, start_time[i], RANGE_ARGS{{NULL,NULL}}, 1))
			return USP_FAULT_INVALID_ARGUMENT;

		if (bbfdm_validate_unsignedInt(ctx, end_time[i], RANGE_ARGS{{NULL,NULL}}, 1))
			return USP_FAULT_INVALID_ARGUMENT;

		if (DM_STRLEN(max_retries[i]) && bbfdm_validate_int(ctx, max_retries[i], RANGE_ARGS{{"-1","10"}}, 1))
			return USP_FAULT_INVALID_ARGUMENT;

		if (bbfdm_validate_string(ctx, mode[i], -1, -1, FW_Mode, NULL))
			return USP_FAULT_INVALID_ARGUMENT;

		if (DM_STRTOL(start_time[i]) > DM_STRTOL(end_time[i]))
			return USP_FAULT_INVALID_ARGUMENT;

		if (i != 0 && DM_STRTOL(end_time[i - 1]) > DM_STRTOL(start_time[i]))
			return USP_FAULT_INVALID_ARGUMENT;

		last_idx++;
	}

	char *bank_id = get_fwbank_option_value(data, "id");

	if (!DM_STRLEN(bank_id))
		return USP_FAULT_COMMAND_FAILURE;

	if (DM_STRLEN(start_time[0])) {
		// cppcheck-suppress cert-MSC24-C
		FILE *file = fopen(CRONTABS_ROOT, "a");
		if (!file)
			return USP_FAULT_COMMAND_FAILURE;

		for (int i = 0; i < MAX_TIME_WINDOW && DM_STRLEN(start_time[i]); i++) {
			char buffer[512] = {0};
			time_t t_time = time(NULL);
			long int start_t = (DM_STRTOL(start_time[i]) > 60) ? DM_STRTOL(start_time[i]) : 60;
			t_time += start_t;
			struct tm *tm_local = localtime(&t_time);

			snprintf(buffer, sizeof(buffer), "%d %d %d %d * sh %s '%s' '%s' '%ld' '%d' '%s' '%s'\n",
											tm_local->tm_min,
											tm_local->tm_hour,
											tm_local->tm_mday,
											tm_local->tm_mon + 1,
											ACTIVATE_HANDLER_FILE,
											mode[i],
											bank_id,
											(DM_STRTOL(end_time[i]) - DM_STRTOL(start_time[i])),
											(i == last_idx),
											user_message[i],
											max_retries[i]);

			fprintf(file, "%s", buffer);
		}

		fclose(file);

		res = dmcmd_no_wait("/etc/init.d/cron", 1, "restart");
	} else {
		if (!fwbank_set_bootbank(bank_id))
			return USP_FAULT_COMMAND_FAILURE;

		bbfdm_task_fork(_exec_reboot, NULL, NULL, NULL);
	}

	return res ? USP_FAULT_COMMAND_FAILURE : 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.FirmwareImage.{i}. *** */
DMLEAF tDeviceInfoFirmwareImageParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"Alias", &DMWRITE, DMT_STRING, get_DeviceInfoFirmwareImage_Alias, set_DeviceInfoFirmwareImage_Alias, BBFDM_BOTH, DM_FLAG_UNIQUE|DM_FLAG_LINKER},
{"Name", &DMREAD, DMT_STRING, get_DeviceInfoFirmwareImage_Name, NULL, BBFDM_BOTH},
{"Version", &DMREAD, DMT_STRING, get_DeviceInfoFirmwareImage_Version, NULL, BBFDM_BOTH},
{"Available", &DMWRITE, DMT_BOOL, get_DeviceInfoFirmwareImage_Available, set_DeviceInfoFirmwareImage_Available, BBFDM_BOTH},
{"Status", &DMREAD, DMT_STRING, get_DeviceInfoFirmwareImage_Status, NULL, BBFDM_BOTH},
{"BootFailureLog", &DMREAD, DMT_STRING, get_empty, NULL, BBFDM_BOTH},
{"Download()", &DMASYNC, DMT_COMMAND, get_operate_args_DeviceInfoFirmwareImage_Download, operate_DeviceInfoFirmwareImage_Download, BBFDM_USP},
{"Activate()", &DMASYNC, DMT_COMMAND, get_operate_args_DeviceInfoFirmwareImage_Activate, operate_DeviceInfoFirmwareImage_Activate, BBFDM_USP},
{0}
};
