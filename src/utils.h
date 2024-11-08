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

#ifndef __UTILS_H
#define __UTILS_H

#include <libbbfdm-api/dmcommon.h>

bool validate_checksum_value(const char *file_path, const char *checksum_algorithm, const char *checksum);
bool validate_file_system_size(const char *file_size);
bool validate_server_response_code(const char *url, int response_code);

void send_transfer_complete_event(const char *command, const char *obj_path, const char *transfer_url,
	char *fault_string, time_t start_t, time_t complete_t, const char *commandKey, const char *transfer_type);

int sysmngr_uci_get(const char *package, const char *section, const char *option, const char *default_value, char *buffer, size_t buffer_size);
int sysmngr_uci_set(const char *package, const char *section, const char *option, const char *value);
int sysmngr_uci_delete(struct uci_context *uci_ctx, const char *package, const char *section);

typedef void (*sysmngr_ubus_cb)(struct ubus_request *req, int type, struct blob_attr *msg);
typedef void (*sysmngr_ubus_async_cb)(struct ubus_request *req, int ret);

int sysmngr_ubus_invoke_async(struct ubus_context *ubus_ctx, const char *obj, const char *method, struct blob_attr *msg,
			    sysmngr_ubus_cb data_callback, sysmngr_ubus_async_cb complete_callback);

int sysmngr_get_uptime(void);

#endif //__UTILS_H
