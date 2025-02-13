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

void send_transfer_complete_event(struct ubus_context *ubus_ctx, const char *command, const char *obj_path, const char *transfer_url,
	char *fault_string, time_t start_t, time_t complete_t, const char *commandKey, const char *transfer_type);

int sysmngr_get_uptime(void);

void sysmngr_generate_critical_log_file(const char *log_path, const char *log_name, bool critical_state);

#endif //__UTILS_H
