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

#define TCP_WMEM_PATH "/proc/sys/net/ipv4/tcp_wmem"
#define TCP_CONGESTION_CONTROL_PATH "/proc/sys/net/ipv4/tcp_congestion_control"

/*************************************************************
* COMMON FUNCTIONS
**************************************************************/
static int read_file_content(const char *file_path, char *buffer, size_t buffer_size)
{
	int fd = open(file_path, O_RDONLY);
	if (fd == -1)
		return -1;

	ssize_t bytes_read = read(fd, buffer, buffer_size - 1);
	if (bytes_read == -1) {
		close(fd);
		return -1;
	}

	buffer[bytes_read] = '\0';  // Null-terminate the buffer
	close(fd);

	return 0;
}

static int read_tcp_write_memory_limits(int *min_size, int *default_size, int *max_size)
{
	char buffer[128] = {0};

	// Read TCP memory settings
	if (read_file_content(TCP_WMEM_PATH, buffer, sizeof(buffer)) == -1)
		return -1;

	// Parse the memory sizes from the file contents
	if (sscanf(buffer, "%d %d %d", min_size, default_size, max_size) != 3)
		return -1;

	return 0;
}

static int get_supported_congestion_controls(char *output, size_t output_size)
{
	const char *delim = " ";
	char *token, buffer[128];
	size_t pos = 0;

	// Read available congestion control algorithms
	if (read_file_content(TCP_CONGESTION_CONTROL_PATH, buffer, sizeof(buffer)) == -1)
		return -1;

	// Parse the supported algorithms and store them in the output buffer
	token = strtok(buffer, delim);

	while (token != NULL) {
		if (strstr(token, "reno"))
			pos += snprintf(&output[pos], output_size - pos, "Reno,");
		else if (strstr(token, "cubic"))
			pos += snprintf(&output[pos], output_size - pos, "Cubic,");
		else if (strstr(token, "newreno"))
			pos += snprintf(&output[pos], output_size - pos, "New Reno,");
		else if (strstr(token, "vegas"))
			pos += snprintf(&output[pos], output_size - pos, "Vegas,");
		else if (strstr(token, "tahoe"))
			pos += snprintf(&output[pos], output_size - pos, "Tahoe,");

		token = strtok(NULL, delim);
	}

	// Remove the trailing comma, if any
	if (pos > 0)
		output[pos - 1] = '\0';

	return 0;
}

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_DeviceInfoNetworkProperties_MaxTCPWindowSize(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int min = 0, def = 0, max = 0;

	int res = read_tcp_write_memory_limits(&min, &def, &max);
	dmasprintf(value, "%d", !res ? max : 0);
	return 0;
}

static int get_DeviceInfoNetworkProperties_TCPImplementation(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char list[256] = {0};

	int res = get_supported_congestion_controls(list, sizeof(list));
	*value = dmstrdup(!res ? list : "");
	return 0;
}

#ifdef SYSMNGR_VENDOR_EXTENSIONS
static int get_Connections_MaxConnections(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char val[32] = {'\0'};
	dm_read_sysfs_file("/proc/sys/net/netfilter/nf_conntrack_max", val, sizeof(val));
	if ('\0' == val[0]) {
		*value = dmstrdup("-1");
	} else {
		*value = dmstrdup(val);
	}
	return 0;
}

static int get_Connections_ActiveConnections(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char val[32] = {'\0'};
	dm_read_sysfs_file("/proc/sys/net/netfilter/nf_conntrack_count", val, sizeof(val));
	if ('\0' == val[0]) {
		*value = dmstrdup("-1");
	} else {
		*value = dmstrdup(val);
	}
	return 0;
}
#endif

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.NetworkProperties. *** */
DMLEAF tDeviceInfoNetworkPropertiesParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"MaxTCPWindowSize", &DMREAD, DMT_UNINT, get_DeviceInfoNetworkProperties_MaxTCPWindowSize, NULL, BBFDM_BOTH},
{"TCPImplementation", &DMREAD, DMT_STRING, get_DeviceInfoNetworkProperties_TCPImplementation, NULL, BBFDM_BOTH},

#ifdef SYSMNGR_VENDOR_EXTENSIONS
{CUSTOM_PREFIX"MaxConnections", &DMREAD, DMT_INT, get_Connections_MaxConnections, NULL, BBFDM_BOTH},
{CUSTOM_PREFIX"ActiveConnections", &DMREAD, DMT_INT, get_Connections_ActiveConnections, NULL, BBFDM_BOTH},
#endif

{0}
};
