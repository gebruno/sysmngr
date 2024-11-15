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

int sysmngr_uci_get(const char *package, const char *section, const char *option, const char *default_value, char *buffer, size_t buffer_size)
{
	struct uci_ptr ptr = {0};
	char uci_str[128] = {0};

	if (!package || !section || !option || !default_value || !buffer || !buffer_size)
		return -1;

	struct uci_context *ctx = uci_alloc_context();
	if (!ctx) {
		BBF_ERR("UCI context allocation failed");
		return -1;
	}

	snprintf(uci_str, sizeof(uci_str), "%s.%s.%s", package, section, option);

	if (uci_lookup_ptr(ctx, &ptr, uci_str, true) != UCI_OK || ptr.o == NULL) {
		snprintf(buffer, buffer_size, "%s", default_value);
		uci_free_context(ctx);
		return -1;
	}

	snprintf(buffer, buffer_size, "%s", ptr.o->v.string);
	uci_free_context(ctx);
	return 0;
}

int sysmngr_uci_set(const char *package, const char *section, const char *option, const char *value)
{
	struct uci_ptr ptr = {0};
	char uci_str[128] = {0};

	if (!package || !section || !value)
		return -1;

	struct uci_context *ctx = uci_alloc_context();
	if (!ctx) {
		BBF_ERR("UCI context allocation failed");
		return -1;
	}

	snprintf(uci_str, sizeof(uci_str), "%s.%s%s%s=%s",
			package,
			section,
			option ? "." : "",
			option ? option : "",
			value);

	if (uci_lookup_ptr(ctx, &ptr, uci_str, true) != UCI_OK ||
		uci_set(ctx, &ptr) != UCI_OK ||
		uci_save(ctx, ptr.p) != UCI_OK ||
		uci_commit(ctx, &ptr.p, false) != UCI_OK) {

		uci_free_context(ctx);
		return -1;
	}

	uci_free_context(ctx);
	return 0;
}

int sysmngr_uci_delete(struct uci_context *uci_ctx, const char *package, const char *section)
{
	struct uci_ptr ptr = {0};
	char uci_str[64] = {0};

	if (!package || !section)
		return -1;

	snprintf(uci_str, sizeof(uci_str), "%s.%s", package, section);

	if (uci_lookup_ptr(uci_ctx, &ptr, uci_str, true) != UCI_OK ||
		uci_delete(uci_ctx, &ptr) != UCI_OK ||
		uci_save(uci_ctx, ptr.p) != UCI_OK) {

		return -1;
	}

	return 0;
}

int sysmngr_ubus_invoke_sync(const char *obj, const char *method, struct blob_attr *msg, sysmngr_ubus_cb data_callback, void *callback_args)
{
	uint32_t id;
	int rc = 0;

	struct ubus_context *ubus_ctx = ubus_connect(NULL);
	if (!ubus_ctx) {
		BBF_ERR("Failed to connect with ubus, error: '%d'", errno);
		return -1;
	}

	if (!ubus_lookup_id(ubus_ctx, obj, &id)) {
		rc = ubus_invoke(ubus_ctx, id, method, msg, data_callback, callback_args, 5000);
	} else {
		BBF_ERR("Failed to lookup ubus object: '%s'", obj);
		rc = -1;
	}

	ubus_free(ubus_ctx);

	return rc;
}


int sysmngr_ubus_invoke_async(struct ubus_context *ubus_ctx, const char *obj, const char *method, struct blob_attr *msg,
			    sysmngr_ubus_cb data_callback, sysmngr_ubus_async_cb complete_callback)
{
	struct ubus_request *req = NULL;
	uint32_t id;

	if (ubus_ctx == NULL) {
		BBF_ERR("Failed to connect with ubus, error: '%d'", errno);
		return -1;
	}

	if (ubus_lookup_id(ubus_ctx, obj, &id)) {
		BBF_ERR("Failed to lookup ubus object: '%s'", obj);
		return -1;
	}

	req = (struct ubus_request *)calloc(1, sizeof(struct ubus_request));
	if (req == NULL) {
		BBF_ERR("failed to allocate memory for ubus request");
		return -1;
	}

	if (ubus_invoke_async(ubus_ctx, id, method, msg, req)) {
		BBF_ERR("ubus async call failed for object: '%s', method: '%s'", obj, method);
		FREE(req);
		return -1;
	}

	if (data_callback)
		req->data_cb = data_callback;

	if (complete_callback)
		req->complete_cb = complete_callback;

	ubus_complete_request_async(ubus_ctx, req);
	return 0;
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
