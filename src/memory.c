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

#define DEFAULT_POLLING_INTERVAL "60"
#define DEFAULT_CRITICAL_RISE_THRESHOLD "80"
#define DEFAULT_CRITICAL_FALL_THRESHOLD "60"
#define DEFAULT_CRITICAL_MEMORY_LOG_PATH "/var/log/critical_memory.log"

typedef struct mem_info {
    unsigned long mem_total;
    unsigned long mem_free;
    unsigned long buffers;
    unsigned long cached;
    unsigned long sreclaimable;
} mem_info;

typedef struct memory_ctx {
	struct uloop_timeout memory_timer;
	bool enable;
	bool enable_critical_log;
	unsigned int polling_interval;
	unsigned int critical_rise_threshold;
	unsigned int critical_fall_threshold;
	time_t critical_rise_time;
	time_t critical_fall_time;
	char log_file[512];
} memory_ctx;

static memory_ctx g_memory_ctx = {0};

/*************************************************************
* COMMON FUNCTIONS
**************************************************************/
int sysmngr_meminfo(mem_info *info)
{
	FILE *f = NULL;
	char *key = NULL, *val = NULL;
	char line[256];

	// cppcheck-suppress cert-MSC24-C
	if ((f = fopen("/proc/meminfo", "r")) == NULL) {
		BBF_ERR("Failed to open '/proc/meminfo' for reading memory info.");
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		key = strtok(line, " :");
		val = strtok(NULL, " ");

		if (!key || !val)
			continue;

		if (!strcasecmp(key, "MemTotal"))
			info->mem_total = strtol(val, NULL, 10);
		else if (!strcasecmp(key, "MemFree"))
			info->mem_free = strtol(val, NULL, 10);
		else if (!strcasecmp(key, "Buffers"))
			info->buffers = strtol(val, NULL, 10);
		else if (!strcasecmp(key, "Cached"))
			info->cached = strtol(val, NULL, 10);
		else if (!strcasecmp(key, "SReclaimable"))
			info->sreclaimable = strtol(val, NULL, 10);
	}

	fclose(f);
	return 0;
}

static unsigned int calculate_memory_utilization(void)
{
	mem_info info = {0};

	if (sysmngr_meminfo(&info) != 0) {
		BBF_ERR("Failed to retrieve memory information for utilization calculation");
		return 0;
	}

	unsigned long used_mem = info.mem_total - (info.mem_free + info.buffers + info.cached + info.sreclaimable);

	return (unsigned int)((used_mem * 100) / info.mem_total);
}

static void send_memory_critical_state_event(unsigned int mem_utilization)
{
	struct blob_buf bb = {0};
	char buf[32] = {0};

	snprintf(buf, sizeof(buf), "%u", mem_utilization);

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	blobmsg_add_string(&bb, "name", "Device.DeviceInfo.MemoryStatus.MemoryMonitor.MemoryCriticalState!");

	void *arr = blobmsg_open_array(&bb, "input");

	void *table = blobmsg_open_table(&bb, NULL);
	blobmsg_add_string(&bb, "path", "MemUtilization");
	blobmsg_add_string(&bb, "data", buf);
	blobmsg_add_string(&bb, "type", DMT_TYPE[DMT_UNINT]);
	blobmsg_close_table(&bb, table);

	blobmsg_close_array(&bb, arr);

	if (sysmngr_ubus_invoke_sync("bbfdm", "notify_event", bb.head, NULL, NULL)) {
		BBF_ERR("Failed to send 'MemoryCriticalState!' event");
	} else {
		BBF_DEBUG("'MemoryCriticalState!' event sent successfully with utilization at %u%%.", mem_utilization);
	}

	blob_buf_free(&bb);
}

static void run_memory_monitor(void)
{
	unsigned int mem_utilization = calculate_memory_utilization();
	char buf[32] = {0};

	if ((mem_utilization > g_memory_ctx.critical_rise_threshold) &&
		(g_memory_ctx.critical_fall_time >= g_memory_ctx.critical_rise_time)) {

		BBF_ERR("Memory utilization reached critical threshold: %u%% !!!!!!!!", mem_utilization);

		// Update CriticalRiseTimeStamp to the current time
		g_memory_ctx.critical_rise_time = time(NULL);
		snprintf(buf, sizeof(buf), "%ld", (long int)g_memory_ctx.critical_rise_time);
		sysmngr_uci_set("sysmngr", "memory", "critical_rise_time", buf);

		if (g_memory_ctx.enable_critical_log) {
			// Generate log into the vendor log file referenced by 'VendorLogFileRef' parameter indicating critical condition is reached
			sysmngr_generate_critical_log_file(g_memory_ctx.log_file, "Memory", true);
		}

		// Send 'MemoryCriticalState!' event
		send_memory_critical_state_event(mem_utilization);
	}

	if ((mem_utilization < g_memory_ctx.critical_fall_threshold) &&
		(g_memory_ctx.critical_rise_time > g_memory_ctx.critical_fall_time)) {

		BBF_ERR("Memory utilization has fallen below critical threshold: %u%% !!!!!!!!", mem_utilization);

		// Update CriticalFallTimeStamp to the current time
		g_memory_ctx.critical_fall_time = time(NULL);
		snprintf(buf, sizeof(buf), "%ld", (long int)g_memory_ctx.critical_fall_time);
		sysmngr_uci_set("sysmngr", "memory", "critical_fall_time", buf);

		if (g_memory_ctx.enable_critical_log) {
			// Generate log into the vendor log file referenced by 'VendorLogFileRef' parameter indicating that the critical condition is no longer present
			sysmngr_generate_critical_log_file(g_memory_ctx.log_file, "Memory", false);
		}
	}

	BBF_INFO("Next memory monitor check scheduled in %d sec...", g_memory_ctx.polling_interval);
	uloop_timeout_set(&g_memory_ctx.memory_timer, g_memory_ctx.polling_interval * 1000);
}

static void memory_timer_callback(struct uloop_timeout *timeout)
{
	run_memory_monitor();
}

static void fill_global_memory_ctx(void)
{
	char buf[16] = {0};

	memset(&g_memory_ctx, 0, sizeof(struct memory_ctx));

	g_memory_ctx.memory_timer.cb = memory_timer_callback;

	sysmngr_uci_get("sysmngr", "memory", "enable", "0", buf, sizeof(buf));
	g_memory_ctx.enable = ((int)strtol(buf, NULL, 10) != 0);
	BBF_DEBUG("Memory Monitor Config: |Enable| |%d|", g_memory_ctx.enable);

	sysmngr_uci_get("sysmngr", "memory", "enable_critical_log", "0", buf, sizeof(buf));
	g_memory_ctx.enable_critical_log = ((int)strtol(buf, NULL, 10) != 0);
	BBF_DEBUG("Memory Monitor Config: |EnableCriticalLog| |%d|", g_memory_ctx.enable_critical_log);

	sysmngr_uci_get("sysmngr", "memory", "polling_interval", DEFAULT_POLLING_INTERVAL, buf, sizeof(buf));
	g_memory_ctx.polling_interval = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |PollingInterval| |%lu|", g_memory_ctx.polling_interval);

	sysmngr_uci_get("sysmngr", "memory", "critical_rise_threshold", DEFAULT_CRITICAL_RISE_THRESHOLD, buf, sizeof(buf));
	g_memory_ctx.critical_rise_threshold = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalRiseThreshold| |%lu|", g_memory_ctx.critical_rise_threshold);

	sysmngr_uci_get("sysmngr", "memory", "critical_fall_threshold", DEFAULT_CRITICAL_FALL_THRESHOLD, buf, sizeof(buf));
	g_memory_ctx.critical_fall_threshold = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalFallThreshold| |%lu|", g_memory_ctx.critical_fall_threshold);

	sysmngr_uci_get("sysmngr", "memory", "critical_rise_time", "0", buf, sizeof(buf));
	g_memory_ctx.critical_rise_time = strtol(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalRiseTimeStamp| |%lu|", g_memory_ctx.critical_rise_time);

	sysmngr_uci_get("sysmngr", "memory", "critical_fall_time", "0", buf, sizeof(buf));
	g_memory_ctx.critical_fall_time = strtol(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalFallTimeStamp| |%lu|", g_memory_ctx.critical_fall_time);

	sysmngr_uci_get("sysmngr", "memory", "file_path", DEFAULT_CRITICAL_MEMORY_LOG_PATH, g_memory_ctx.log_file, sizeof(g_memory_ctx.log_file));
	BBF_DEBUG("Memory Monitor Config: |FilePath| |%s|", g_memory_ctx.log_file);
	if (!file_exists(g_memory_ctx.log_file)) {
		// Create empty file if it doesn't exist
		create_empty_file(g_memory_ctx.log_file);
	}
}

/*************************************************************
* EXTERNAL APIS
**************************************************************/
void sysmngr_memory_init(void)
{
	fill_global_memory_ctx();

	if (!g_memory_ctx.enable) {
		BBF_INFO("Memory monitoring is disabled.");
		return;
	}

	BBF_INFO("Memory monitoring is enabled");
	run_memory_monitor();
}

void sysmngr_memory_clean(void)
{
	uloop_timeout_cancel(&g_memory_ctx.memory_timer);
	BBF_INFO("Memory monitoring process stopped");
}

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_DeviceInfoMemoryStatusMemoryMonitor_Enable(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "enable", "0");
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_Enable(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	bool b;

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_boolean(ctx, value))
				return FAULT_9007;
			break;
		case VALUESET:
			string_to_bool(value, &b);
			dmuci_set_value("sysmngr", "memory", "enable", b ? "1" : "0");
			break;
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_MemUtilization(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", calculate_memory_utilization());
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_PollingInterval(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "polling_interval", DEFAULT_POLLING_INTERVAL);
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_PollingInterval(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,NULL}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "memory", "polling_interval", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "critical_rise_threshold", DEFAULT_CRITICAL_RISE_THRESHOLD);
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,"100"}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "memory", "critical_rise_threshold", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "critical_fall_threshold", DEFAULT_CRITICAL_FALL_THRESHOLD);
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,"100"}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "memory", "critical_fall_threshold", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseTimeStamp(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *rise_time = NULL;

	dmuci_get_option_value_string("sysmngr", "memory", "critical_rise_time", &rise_time);

	return dm_time_utc_format(DM_STRTOL(rise_time), value);
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallTimeStamp(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *fall_time = NULL;

	dmuci_get_option_value_string("sysmngr", "memory", "critical_fall_time", &fall_time);

	return dm_time_utc_format(DM_STRTOL(fall_time), value);
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_EnableCriticalLog(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "enable_critical_log", "0");
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_EnableCriticalLog(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	bool b;

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_boolean(ctx, value))
				return FAULT_9007;
			break;
		case VALUESET:
			string_to_bool(value, &b);
			dmuci_set_value("sysmngr", "memory", "enable_critical_log", b ? "1" : "0");
			break;
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_VendorLogFileRef(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *file_path = dmuci_get_option_value_fallback_def("sysmngr", "memory", "file_path", DEFAULT_CRITICAL_MEMORY_LOG_PATH);

	if (file_exists(file_path)) {
		char file_uri[512] = {0};

		// if there is a path, then prepend file:// to it to comply with bbf requirement of file URI
		snprintf(file_uri, sizeof(file_uri), "file://%s", file_path);

		// get the vendor file path
		_bbfdm_get_references(ctx, "Device.DeviceInfo.VendorLogFile.", "Name", file_uri, value);
	}
	return 0;
}

static int get_DeviceInfoMemoryStatusMemoryMonitor_FilePath(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "memory", "file_path", DEFAULT_CRITICAL_MEMORY_LOG_PATH);
	return 0;
}

static int set_DeviceInfoMemoryStatusMemoryMonitor_FilePath(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	char *file_path = dmuci_get_option_value_fallback_def("sysmngr", "memory", "file_path", DEFAULT_CRITICAL_MEMORY_LOG_PATH);

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, value, -1, -1, NULL, NULL))
				return FAULT_9007;

			// Restriction: The path in `value` must either:
			// - Start with "/var/log" for non-persistent logs, or
			// - Start with "/log/" for persistent logs.
			// Additionally, the path should not contain any '..' sequences
			// to prevent directory traversal or invalid file paths.
			if (!((strncmp(value, "/var/log", 8) == 0 || strncmp(value, "/log/", 5) == 0) && !strstr(value, ".."))) {
				bbfdm_set_fault_message(ctx, "");
				return FAULT_9007;
			}

			break;
		case VALUESET:
			if (file_exists(file_path)) {
				struct uci_section *dmmap_sec = NULL;
				char file_uri[512] = {0};

				if (rename(file_path, value) != 0) {
					bbfdm_set_fault_message(ctx, "Can't rename file from '%s' -> '%s'", file_path, value);
					return FAULT_9007;
				}

				// Update VendorLogFile dmmap section
				snprintf(file_uri, sizeof(file_uri), "file://%s", file_path);
				dmmap_sec = get_dup_section_in_dmmap_opt("dmmap", "vendorlog", "log_file", file_uri);

				snprintf(file_uri, sizeof(file_uri), "file://%s", value);
				dmuci_set_value_by_section(dmmap_sec, "log_file", file_uri);
			}

			dmuci_set_value("sysmngr", "memory", "file_path", value);
			break;
	}
	return 0;
}

static int get_memory_status_total(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	mem_info info = {0};

	sysmngr_meminfo(&info);

	dmasprintf(value, "%lu", info.mem_total);
	return 0;
}

static int get_memory_status_free(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	mem_info info = {0};

	sysmngr_meminfo(&info);

	dmasprintf(value, "%lu", info.mem_free);
	return 0;
}

static int get_memory_status_total_persistent(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct statvfs dinfo;

	if (statvfs("/overlay/", &dinfo) == 0) {
		unsigned int total = (dinfo.f_bsize * dinfo.f_blocks) / 1024;
		dmasprintf(value, "%u", total);
	} else {
		*value = dmstrdup("0");
	}

	return 0;
}

static int get_memory_status_free_persistent(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	struct statvfs dinfo;

	if (statvfs("/overlay/", &dinfo) == 0) {
		unsigned int free = (dinfo.f_bsize * dinfo.f_bavail) / 1024;
		dmasprintf(value, "%u", free);
	} else {
		*value = dmstrdup("0");
	}

	return 0;
}

/*************************************************************
 * EVENTS
 *************************************************************/
static event_args MemoryCriticalState_event_args = {
	.name = "", // This field is left empty because we are not listening to any external events, The system now operates within a single unified daemon,
				// removing the need for separate event listeners. See send_memory_critical_state_event API for details on implementation.
	.param = (const char *[]) {
		"MemUtilization",
		NULL
	}
};

static int get_event_MemoryCriticalState(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&MemoryCriticalState_event_args;
	return 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.MemoryStatus.MemoryMonitor. *** */
DMLEAF tDeviceInfoMemoryStatusMemoryMonitorParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"Enable", &DMWRITE, DMT_BOOL, get_DeviceInfoMemoryStatusMemoryMonitor_Enable, set_DeviceInfoMemoryStatusMemoryMonitor_Enable, BBFDM_BOTH},
{"MemUtilization", &DMREAD, DMT_UNINT, get_DeviceInfoMemoryStatusMemoryMonitor_MemUtilization, NULL, BBFDM_BOTH},
{"PollingInterval", &DMWRITE, DMT_UNINT, get_DeviceInfoMemoryStatusMemoryMonitor_PollingInterval, set_DeviceInfoMemoryStatusMemoryMonitor_PollingInterval, BBFDM_BOTH},
{"CriticalRiseThreshold", &DMWRITE, DMT_UNINT, get_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseThreshold, set_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseThreshold, BBFDM_BOTH},
{"CriticalFallThreshold", &DMWRITE, DMT_UNINT, get_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallThreshold, set_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallThreshold, BBFDM_BOTH},
{"CriticalRiseTimeStamp", &DMREAD, DMT_TIME, get_DeviceInfoMemoryStatusMemoryMonitor_CriticalRiseTimeStamp, NULL, BBFDM_BOTH},
{"CriticalFallTimeStamp", &DMREAD, DMT_TIME, get_DeviceInfoMemoryStatusMemoryMonitor_CriticalFallTimeStamp, NULL, BBFDM_BOTH},
{"EnableCriticalLog", &DMWRITE, DMT_BOOL, get_DeviceInfoMemoryStatusMemoryMonitor_EnableCriticalLog, set_DeviceInfoMemoryStatusMemoryMonitor_EnableCriticalLog, BBFDM_BOTH},
{"VendorLogFileRef", &DMREAD, DMT_STRING, get_DeviceInfoMemoryStatusMemoryMonitor_VendorLogFileRef, NULL, BBFDM_BOTH},
{"FilePath", &DMWRITE, DMT_STRING, get_DeviceInfoMemoryStatusMemoryMonitor_FilePath, set_DeviceInfoMemoryStatusMemoryMonitor_FilePath, BBFDM_BOTH},
{"MemoryCriticalState!", &DMREAD, DMT_EVENT, get_event_MemoryCriticalState, NULL, BBFDM_USP},
{0}
};

/* *** Device.DeviceInfo.MemoryStatus. *** */
DMOBJ tDeviceInfoMemoryStatusObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type */
{"MemoryMonitor", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoMemoryStatusMemoryMonitorParams, NULL, BBFDM_BOTH},
{0}
};

DMLEAF tDeviceInfoMemoryStatusParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"Total", &DMREAD, DMT_UNINT, get_memory_status_total, NULL, BBFDM_BOTH},
{"Free", &DMREAD, DMT_UNINT, get_memory_status_free, NULL, BBFDM_BOTH},
{"TotalPersistent", &DMREAD, DMT_UNINT, get_memory_status_total_persistent, NULL, BBFDM_BOTH},
{"FreePersistent", &DMREAD, DMT_UNINT, get_memory_status_free_persistent, NULL, BBFDM_BOTH},
{0}
};
