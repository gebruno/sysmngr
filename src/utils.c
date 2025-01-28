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

#include <openssl/sha.h>
#include <openssl/evp.h>

#define CRITICAL_STATE_LOGGER_PATH "/etc/sysmngr/critical_state_logger.sh"

static bool validate_hash_value(const char *algo, const char *file_path, const char *checksum)
{
	unsigned char buffer[1024 * 16] = {0};
	char hash[BUFSIZ] = {0};
	bool res = false;
	unsigned int bytes = 0;
	FILE *file;

	EVP_MD_CTX *mdctx;
	const EVP_MD *md;
	unsigned char md_value[EVP_MAX_MD_SIZE];

	// cppcheck-suppress cert-MSC24-C
	file = fopen(file_path, "rb");
	if (!file)
		return false;

	md = EVP_get_digestbyname(algo);
	mdctx = EVP_MD_CTX_create();
	EVP_DigestInit_ex(mdctx, md, NULL);

	if (md == NULL)
		goto end;

	while ((bytes = fread (buffer, 1, sizeof(buffer), file))) {
		EVP_DigestUpdate(mdctx, buffer, bytes);
	}

	bytes = 0;
	EVP_DigestFinal_ex(mdctx, md_value, &bytes);

	for (int i = 0; i < bytes; i++)
		snprintf(&hash[i * 2], sizeof(hash) - (i * 2), "%02x", md_value[i]);

	if (DM_STRCMP(hash, checksum) == 0)
		res = true;

end:
	EVP_MD_CTX_destroy(mdctx);
	EVP_cleanup();

	fclose(file);
	return res;
}

bool validate_checksum_value(const char *file_path, const char *checksum_algorithm, const char *checksum)
{
	if (checksum && *checksum) {

		if (strcmp(checksum_algorithm, "SHA-1") == 0)
			return validate_hash_value("SHA1", file_path, checksum);
		else if (strcmp(checksum_algorithm, "SHA-224") == 0)
			return validate_hash_value("SHA224", file_path, checksum);
		else if (strcmp(checksum_algorithm, "SHA-256") == 0)
			return validate_hash_value("SHA256", file_path, checksum);
		else if (strcmp(checksum_algorithm, "SHA-384") == 0)
			return validate_hash_value("SHA384", file_path, checksum);
		else if (strcmp(checksum_algorithm, "SHA-512") == 0)
			return validate_hash_value("SHA512", file_path, checksum);
		else
			return false;
	}

	return true;
}

bool validate_server_response_code(const char *url, int response_code)
{
	if ((strncmp(url, HTTP_URI, strlen(HTTP_URI)) == 0 && response_code != 200) ||
		(strncmp(url, FTP_URI, strlen(FTP_URI)) == 0 && response_code != 226) ||
		(strncmp(url, FILE_URI, strlen(FILE_URI)) == 0 && response_code != 0) ||
		(strncmp(url, HTTP_URI, strlen(HTTP_URI)) && strncmp(url, FTP_URI, strlen(FTP_URI)) && strncmp(url, FILE_URI, strlen(FILE_URI)))) {
		return false;
	}

	return true;
}

bool validate_file_system_size(const char *file_size)
{
	if (file_size && *file_size) {
		unsigned long f_size = strtoul(file_size, NULL, 10);
		unsigned long fs_available_size = file_system_size("/tmp", FS_SIZE_AVAILABLE);

		if (fs_available_size < f_size)
			return false;
	}

	return true;
}

void send_transfer_complete_event(const char *command, const char *obj_path, const char *transfer_url,
	char *fault_string, time_t start_t, time_t complete_t, const char *commandKey, const char *transfer_type)
{
	char start_time[32] = {0};
	char complete_time[32] = {0};
	struct blob_buf bb;

	strftime(start_time, sizeof(start_time), "%Y-%m-%dT%H:%M:%SZ", gmtime(&start_t));
	strftime(complete_time, sizeof(complete_time), "%Y-%m-%dT%H:%M:%SZ", gmtime(&complete_t));

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	blobmsg_add_string(&bb, "name", "Device.LocalAgent.TransferComplete!");
	void *arr = blobmsg_open_array(&bb, "input");

	fill_blob_param(&bb, "Command", command, DMT_TYPE[DMT_STRING], 0);
	if(commandKey)
		fill_blob_param(&bb, "CommandKey", commandKey, DMT_TYPE[DMT_STRING], 0);
	else
		fill_blob_param(&bb, "CommandKey", "", DMT_TYPE[DMT_STRING], 0);

	fill_blob_param(&bb, "Requestor", "", DMT_TYPE[DMT_STRING], 0);
	fill_blob_param(&bb, "TransferType", transfer_type, DMT_TYPE[DMT_STRING], 0);
	fill_blob_param(&bb, "Affected", obj_path, DMT_TYPE[DMT_STRING], 0);
	fill_blob_param(&bb, "TransferURL", transfer_url, DMT_TYPE[DMT_STRING], 0);
	fill_blob_param(&bb, "StartTime", start_time, DMT_TYPE[DMT_STRING], 0);
	fill_blob_param(&bb, "CompleteTime", complete_time, DMT_TYPE[DMT_STRING], 0);

	if (DM_STRLEN(fault_string) == 0) {
		fill_blob_param(&bb, "FaultCode", "0", DMT_TYPE[DMT_STRING], 0);
	} else {
		fill_blob_param(&bb, "FaultCode", "7000", DMT_TYPE[DMT_STRING], 0);
	}

	fill_blob_param(&bb, "FaultString", fault_string, DMT_TYPE[DMT_STRING], 0);
	blobmsg_close_array(&bb, arr);

	dmubus_call_blob_msg_set("bbfdm", "notify_event", &bb);

	blob_buf_free(&bb);
}

int sysmngr_get_uptime(void)
{
	// cppcheck-suppress cert-MSC24-C
	FILE *fp = fopen("/proc/uptime", "r");
	int uptime = 0;

	if (fp != NULL) {
		char *pch = NULL, *spch = NULL, buf[64] = {0};

		if (fgets(buf, sizeof(buf), fp) != NULL) {
			pch = strtok_r(buf, ".", &spch);
			uptime = (pch) ? (int)strtol(pch, NULL, 10) : 0;
		}
		fclose(fp);
	}

	return uptime;
}

void sysmngr_generate_critical_log_file(const char *log_path, const char *type, bool critical_state)
{
	char cmd[1024] = {0};
	char output[256] = {0};

	// sh /etc/sysmngr/critical_state_logger.sh 'CPU' '/var/log/critical_memory.log' 'true'
	snprintf(cmd, sizeof(cmd), "sh %s %s %s %s", CRITICAL_STATE_LOGGER_PATH,
			type, log_path, critical_state ? "true" : false);

	int res = run_cmd(cmd, output, sizeof(output));
	if (!res || strncmp(output, "Success", 7) == 0)
		BBFDM_DEBUG("Critical log generation succeeded: result=%d, output='%s'", res, output);
	else
        BBFDM_DEBUG("Critical log generation failed: result=%d, output='%s'", res, output);
}

