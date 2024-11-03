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

#define CONFIG_BACKUP "/tmp/bbf_config_backup"

/*************************************************************
* COMMON FUNCTIONS
**************************************************************/
static int dmmap_synchronizeVcfInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct uci_section *s = NULL, *stmp = NULL;
	DIR *dir;
	struct dirent *d_file;

	sysfs_foreach_file(DEFAULT_CONFIG_DIR, dir, d_file) {

		if(d_file->d_name[0] == '.')
			continue;

		if (!is_dmmap_section_exist_eq("dmmap", "vcf", "name", d_file->d_name)) {
			dmuci_add_section_bbfdm("dmmap", "vcf", &s);
			dmuci_set_value_by_section(s, "name", d_file->d_name);
			dmuci_set_value_by_section(s, "backup_restore", "1");
		}
	}

	if (dir)
		closedir (dir);

	uci_path_foreach_sections_safe(bbfdm, "dmmap", "vcf", stmp, s) {
		char cfg_path[128] = {0};
		char *cfg_name = NULL;

		dmuci_get_value_by_section_string(s, "name", &cfg_name);

		snprintf(cfg_path, sizeof(cfg_path), "%s%s", DEFAULT_CONFIG_DIR, cfg_name);

		if (!file_exists(cfg_path))
			dmuci_delete_by_section_bbfdm(s, NULL, NULL);
	}

	return 0;
}

static int bbf_config_backup(const char *url, const char *username, const char *password,
		char *config_name, const char *command, const char *obj_path)
{
	int res = 0;
	char fault_msg[128] = {0};
	time_t complete_time = 0;
	time_t start_time = time(NULL);

	// Export config file to backup file
	if (dmuci_export_package(config_name, CONFIG_BACKUP)) {
		snprintf(fault_msg, sizeof(fault_msg), "Failed to export the configurations");
		res = -1;
		goto end;
	}

	// Upload the config file
	long res_code = upload_file(CONFIG_BACKUP, url, username, password);
	complete_time = time(NULL);

	// Check if the upload operation was successful
	if (!validate_server_response_code(url, res_code)) {
		res = -1;
		snprintf(fault_msg, sizeof(fault_msg), "Upload operation is failed, fault code (%ld)", res_code);
	}

end:
	// Send the transfer complete event
	send_transfer_complete_event(command, obj_path, url, fault_msg, start_time, complete_time, NULL, "Upload");

	// Remove temporary file
	if (file_exists(CONFIG_BACKUP) && remove(CONFIG_BACKUP))
		res = -1;

	return res;
}


static int bbf_config_restore(const char *url, const char *username, const char *password,
		const char *file_size, const char *checksum_algorithm, const char *checksum,
		const char *command, const char *obj_path)
{
	char config_restore[256] = {0};
	int res = 0;
	char fault_msg[128] = {0};
	time_t complete_time = 0;
	time_t start_time = time(NULL);

	DM_STRNCPY(config_restore, "/tmp/bbf_config_restore", sizeof(config_restore));

	// Check the file system size if there is sufficient space for downloading the config file
	if (!validate_file_system_size(file_size)) {
		snprintf(fault_msg, sizeof(fault_msg), "Available memory space is less than required for the operation");
		res = -1;
		goto end;
	}

	// Download the firmware image
	long res_code = download_file(config_restore, url, username, password);
	complete_time = time(NULL);

	// Check if the download operation was successful
	if (!validate_server_response_code(url, res_code)) {
		snprintf(fault_msg, sizeof(fault_msg), "Upload operation is failed, fault code (%ld)", res_code);
		res = -1;
		goto end;
	}

	// Validate the CheckSum value according to its algorithm
	if (!validate_checksum_value(config_restore, checksum_algorithm, checksum)) {
		snprintf(fault_msg, sizeof(fault_msg), "Checksum of the downloaded file is mismatched");
		res = -1;
		goto end;
	}

	// Apply config file
	if (dmuci_import(NULL, config_restore)) {
		snprintf(fault_msg, sizeof(fault_msg), "Failed to import the configurations");
		res = -1;
	}

end:
	// Send the transfer complete event
	send_transfer_complete_event(command, obj_path, url, fault_msg, start_time, complete_time, NULL, "Download");

	// Remove temporary file
	if (file_exists(config_restore) && strncmp(url, FILE_URI, strlen(FILE_URI)) && remove(config_restore))
		res = -1;

	return res;
}

/*************************************************************
* ENTRY METHOD
**************************************************************/
int browseVcfInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct dm_data curr_data = {0};
	struct uci_section *s = NULL;
	char *inst = NULL;

	dmmap_synchronizeVcfInst(dmctx, parent_node, prev_data, prev_instance);
	uci_path_foreach_sections(bbfdm, "dmmap", "vcf", s) {

		curr_data.config_section = s;

		inst = handle_instance(dmctx, parent_node, s, "vcf_instance", "vcf_alias");

		if (DM_LINK_INST_OBJ(dmctx, parent_node, (void *)&curr_data, inst) == DM_STOP)
			break;
	}
	return 0;
}

/*************************************************************
* GET & SET PARAM
**************************************************************/
int get_DeviceInfo_VendorConfigFileNumberOfEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseVcfInst);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_vcf_name(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "name", value);
	return 0;
}

static int get_vcf_version(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "version", value);
	return 0;
}

static int get_vcf_date(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	DIR *dir = NULL;
	struct dirent *d_file = NULL;
	char *config_name = NULL;

	*value = dmstrdup("0001-01-01T00:00:00Z");
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "name", &config_name);
	if ((dir = opendir (DEFAULT_CONFIG_DIR)) != NULL) {
		while ((d_file = readdir (dir)) != NULL) {
			if (config_name && DM_STRCMP(config_name, d_file->d_name) == 0) {
				char date[sizeof("AAAA-MM-JJTHH:MM:SSZ")], path[280] = {0};
				struct stat attr;

				snprintf(path, sizeof(path), "%s%s", DEFAULT_CONFIG_DIR, d_file->d_name);
				stat(path, &attr);
				strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", gmtime(&attr.st_mtime));
				*value = dmstrdup(date);
			}
		}
		closedir (dir);
	}
	return 0;
}

static int get_vcf_backup_restore(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "backup_restore", value);
	return 0;
}

static int get_vcf_desc(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "description", value);
	return 0;
}

static int get_vcf_alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	return bbf_get_alias(ctx, ((struct dm_data *)data)->config_section, "vcf_alias", instance, value);
}

static int set_vcf_alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	return bbf_set_alias(ctx, ((struct dm_data *)data)->config_section, "vcf_alias", instance, value);
}

/*************************************************************
 * OPERATE COMMANDS
 *************************************************************/
static operation_args vendor_config_file_backup_args = {
	.in = (const char *[]) {
		"URL",
		"Username",
		"Password",
		NULL
	}
};

static int get_operate_args_DeviceInfoVendorConfigFile_Backup(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&vendor_config_file_backup_args;
	return 0;
}

static int operate_DeviceInfoVendorConfigFile_Backup(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	const char *backup_command = "Backup()";
	char backup_path[256] = {'\0'};
	char *vcf_name = NULL;

	char *ret = DM_STRRCHR(refparam, '.');
	if (!ret)
		return USP_FAULT_INVALID_ARGUMENT;

	if ((ret - refparam + 2) < sizeof(backup_path))
		snprintf(backup_path, ret - refparam + 2, "%s", refparam);

	char *url = dmjson_get_value((json_object *)value, 1, "URL");
	if (url[0] == '\0')
		return USP_FAULT_INVALID_ARGUMENT;

	char *user = dmjson_get_value((json_object *)value, 1, "Username");
	char *pass = dmjson_get_value((json_object *)value, 1, "Password");

	dmuci_get_value_by_section_string(((struct dm_data *)data)->config_section, "name", &vcf_name);

	int res = bbf_config_backup(url, user, pass, vcf_name, backup_command, backup_path);

	return res ? USP_FAULT_COMMAND_FAILURE : 0;
}

static operation_args vendor_config_file_restore_args = {
	.in = (const char *[]) {
		"URL",
		"Username",
		"Password",
		"FileSize",
		"TargetFileName",
		"CheckSumAlgorithm",
		"CheckSum",
		NULL
	}
};

static int get_operate_args_DeviceInfoVendorConfigFile_Restore(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&vendor_config_file_restore_args;
	return 0;
}

static int operate_DeviceInfoVendorConfigFile_Restore(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	const char *restore_command = "Restore()";
	char restore_path[256] = {'\0'};

	char *ret = DM_STRRCHR(refparam, '.');
	if (!ret)
		return USP_FAULT_INVALID_ARGUMENT;

	if ((ret - refparam + 2) < sizeof(restore_path))
		snprintf(restore_path, ret - refparam + 2, "%s", refparam);

	char *url = dmjson_get_value((json_object *)value, 1, "URL");
	if (url[0] == '\0')
		return USP_FAULT_INVALID_ARGUMENT;

	char *user = dmjson_get_value((json_object *)value, 1, "Username");
	char *pass = dmjson_get_value((json_object *)value, 1, "Password");
	char *file_size = dmjson_get_value((json_object *)value, 1, "FileSize");
	char *checksum_algorithm = dmjson_get_value((json_object *)value, 1, "CheckSumAlgorithm");
	char *checksum = dmjson_get_value((json_object *)value, 1, "CheckSum");

	int res = bbf_config_restore(url, user, pass, file_size, checksum_algorithm, checksum, restore_command, restore_path);

	return res ? USP_FAULT_COMMAND_FAILURE : 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.VendorConfigFile.{i}. *** */
DMLEAF tDeviceInfoVendorConfigFileParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"Alias", &DMWRITE, DMT_STRING, get_vcf_alias, set_vcf_alias, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Name", &DMREAD, DMT_STRING, get_vcf_name, NULL, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Version", &DMREAD, DMT_STRING, get_vcf_version, NULL, BBFDM_BOTH},
{"Date", &DMREAD, DMT_TIME, get_vcf_date, NULL, BBFDM_BOTH},
{"Description", &DMREAD, DMT_STRING, get_vcf_desc, NULL, BBFDM_BOTH},
{"UseForBackupRestore", &DMREAD, DMT_BOOL, get_vcf_backup_restore, NULL, BBFDM_BOTH},
{"Backup()", &DMASYNC, DMT_COMMAND, get_operate_args_DeviceInfoVendorConfigFile_Backup, operate_DeviceInfoVendorConfigFile_Backup, BBFDM_USP},
{"Restore()", &DMASYNC, DMT_COMMAND, get_operate_args_DeviceInfoVendorConfigFile_Restore, operate_DeviceInfoVendorConfigFile_Restore, BBFDM_USP},
{0}
};
