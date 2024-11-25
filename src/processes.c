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
#include "processes.h"

#define DEFAULT_CPU_NAME "cpu"
#define DEFAULT_CPU_POLL_INTERVAL "5"
#define DEFAULT_CPU_NUM_SAMPLES "30"
#define DEFAULT_CPU_CRITICAL_RISE_THRESHOLD "80"
#define DEFAULT_CPU_CRITICAL_FALL_THRESHOLD "60"
#define DEFAULT_CPU_CRITICAL_LOG_PATH "/var/log/critical_cpu.log"

typedef struct process_entry {
	struct list_head list;

	char command[256];
	char state[16];
	char pid[8];
	char size[8];
	char priority[8];
	char cputime[8];
} process_entry;

typedef struct jiffy_counts_t {
	unsigned long long usr, nic, sys, idle;
	unsigned long long iowait, irq, softirq, steal;
	unsigned long long total_time;
	unsigned long long idle_time;
	unsigned long long sys_time;
	unsigned long long busy_time;
} jiffy_counts_t;

typedef struct process_ctx {
	struct ubus_context *ubus_ctx;
	struct uloop_timeout instance_timer;
	struct list_head list;
	int refresh_interval;
	int process_num;
} process_ctx;

typedef struct cpu_info {
	struct uloop_timeout cpu_timer;
	jiffy_counts_t jiffy;
	bool enable;
	bool enable_critical_log;
	bool full_samples_reached;
	unsigned int poll_interval;
	unsigned int critical_rise_threshold;
	unsigned int critical_fall_threshold;
	unsigned int *user_utilization_samples;
	unsigned int *system_utilization_samples;
	unsigned int *idle_utilization_samples;
	unsigned int *utilization_samples;
	unsigned int num_samples;
	time_t critical_rise_time;
	time_t critical_fall_time;
	size_t sample_index;
	char log_file[512];
} cpu_info_t;

static process_ctx g_process_ctx = {0};
static cpu_info_t g_cpu_info = {0};

/*************************************************************
* COMMON FUNCTIONS
**************************************************************/
static void get_jif_val(jiffy_counts_t *p_jif)
{
	FILE *file = NULL;
	char line[128];
	int ret;

	if ((file = fopen("/proc/stat", "r"))) {
		while(fgets(line, sizeof(line), file) != NULL)
		{
			remove_new_line(line);
			ret = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &p_jif->usr, &p_jif->nic, &p_jif->sys, &p_jif->idle,
				&p_jif->iowait, &p_jif->irq, &p_jif->softirq, &p_jif->steal);

			if (ret >= 4) {
				p_jif->total_time = p_jif->usr + p_jif->nic + p_jif->sys + p_jif->idle
					+ p_jif->iowait + p_jif->irq + p_jif->softirq + p_jif->steal;

				p_jif->sys_time = p_jif->sys + p_jif->irq + p_jif->softirq;

				p_jif->idle_time = p_jif->idle + p_jif->iowait;

				p_jif->busy_time = p_jif->total_time - p_jif->idle_time;
				break;
			}
		}
		fclose(file);
	}
}

static unsigned int get_cpu_load(jiffy_counts_t *prev_jif, jiffy_counts_t *cur_jif)
{
	unsigned total_diff, cpu;

	total_diff = (unsigned)(cur_jif->total_time - prev_jif->total_time);

	if (total_diff == 0)
		total_diff = 1;

	cpu = 100 * (unsigned)(cur_jif->busy_time - prev_jif->busy_time) / total_diff;

	return cpu;
}

static unsigned int get_cpu_usage(void)
{
	jiffy_counts_t prev_jif = {0};
	jiffy_counts_t cur_jif = {0};

	get_jif_val(&prev_jif);
	usleep(100000);
	get_jif_val(&cur_jif);

	return get_cpu_load(&prev_jif, &cur_jif);
}

static char *get_proc_state(char state)
{
	switch(state) {
		case 'R':
			return "Running";
		case 'S':
			return "Sleeping";
		case 'T':
			return "Stopped";
		case 'D':
			return "Uninterruptible";
		case 'Z':
			return "Zombie";
		case 'I':
			return "Idle";
	};

	return "Idle";
}

static void procps_get_cmdline(char *buf, int bufsz, const char *pid, const char *comm)
{
	char filename[270] = {0};

	snprintf(filename, sizeof(filename), "/proc/%s/cmdline", pid);

	int sz = dm_file_to_buf(filename, buf, bufsz);
	if (sz > 0) {
		const char *base;
		int comm_len;

		while (--sz >= 0 && buf[sz] == '\0')
			continue;

		/* Prevent basename("process foo/bar") = "bar" */
		strchrnul(buf, ' ')[0] = '\0';
		base = basename(buf); /* before we replace argv0's NUL with space */
		while (sz >= 0) {
			if ((unsigned char)(buf[sz]) < ' ')
				buf[sz] = ' ';
			sz--;
		}

		if (base[0] == '-') /* "-sh" (login shell)? */
			base++;

		/* If comm differs from argv0, prepend "{comm} ".
		 * It allows to see thread names set by prctl(PR_SET_NAME).
		 */
		if (!comm)
			return;

		comm_len = strlen(comm);
		/* Why compare up to comm_len?
		 * Well, some processes rewrite argv, and use _spaces_ there
		 * while rewriting. (KDE is observed to do it).
		 * I prefer to still treat argv0 "process foo bar"
		 * as 'equal' to comm "process".
		 */
		if (strncmp(base, comm, comm_len) != 0) {
			comm_len += 3;
			if (bufsz > comm_len)
				memmove(buf + comm_len, buf, bufsz - comm_len);
			snprintf(buf, bufsz, "{%s}", comm);
			if (bufsz <= comm_len)
				return;
			buf[comm_len - 1] = ' ';
			buf[bufsz - 1] = '\0';
		}
	} else {
		snprintf(buf, bufsz, "[%s]", comm ? comm : "?");
	}
}

static void broadcast_add_del_event(int diff)
{
	struct blob_buf bb = {0};
	char method_name[64] = {0};

	// On the first run, add and delete events are managed by the instance refresh mechanism defined in bbfdm
	if (g_process_ctx.process_num == 0)
		return;

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	void *a = blobmsg_open_array(&bb, "instances");

	for (int i = 0; i < abs(diff); i++) {
		char obj_path[256] = {0};

		snprintf(obj_path, sizeof(obj_path), "Device.DeviceInfo.ProcessStatus.Process.%d", (diff > 0) ? g_process_ctx.process_num + i + 1 : g_process_ctx.process_num - i);
		blobmsg_add_string(&bb, NULL, obj_path);
		BBF_DEBUG("#%s:: %s #", (diff > 0) ? "Add" : "Del", obj_path);
	}
	blobmsg_close_array(&bb, a);

	snprintf(method_name, sizeof(method_name), "%s.%s", "bbfdm", (diff > 0) ? "AddObj" : "DelObj");

	ubus_send_event(g_process_ctx.ubus_ctx, method_name, bb.head);

	blob_buf_free(&bb);
}

static void init_process_list(void)
{
	struct dirent *entry = NULL;
	DIR *dir = NULL;
	unsigned int cur_process_num = 0;

	dir = opendir("/proc");
	if (dir == NULL)
		return;

	BBF_INFO("Init process list");

	while ((entry = readdir(dir)) != NULL) {
		struct stat stats = {0};
		char buf[1024], fstat[288], command[256], comm[32];
		char bsize[32], cputime[32], priori[32], state;
		unsigned long stime, utime, vsize;
		int priority, n;

		int digit = entry->d_name[0] - '0';
		if (digit < 0 || digit > 9)
			continue;

		snprintf(fstat, sizeof(fstat), "/proc/%s/stat", entry->d_name);
		if (stat(fstat, &stats))
			continue;

		n = dm_file_to_buf(fstat, buf, sizeof(buf));
		if (n < 0)
			continue;

		char *comm2 = strrchr(buf, ')'); /* split into "PID (cmd" and "<rest>" */
		if (!comm2) /* sanity check */
		  continue;

		comm2[0] = '\0';
		char *comm1 = strchr(buf, '(');
		if (!comm1) /* sanity check */
		  continue;

		DM_STRNCPY(comm, comm1 + 1, sizeof(comm));

		n = sscanf(comm2 + 2,			  /* Flawfinder: ignore */ \
				"%c %*u "                 /* state, ppid */
				"%*u %*u %*d %*s "        /* pgid, sid, tty, tpgid */
				"%*s %*s %*s %*s %*s "    /* flags, min_flt, cmin_flt, maj_flt, cmaj_flt */
				"%lu %lu "                /* utime, stime */
				"%*u %*u %d "             /* cutime, cstime, priority */
				"%*d "                    /* niceness */
				"%*s %*s "                /* timeout, it_real_value */
				"%*s "                    /* start_time */
				"%lu "                    /* vsize */
				,
				&state,
				&utime, &stime,
				&priority,
				&vsize
			  );

		if (n != 5)
			continue;

		procps_get_cmdline(command, sizeof(command), entry->d_name, comm);

		snprintf(cputime, sizeof(cputime), "%lu", ((stime / sysconf(_SC_CLK_TCK)) + (utime / sysconf(_SC_CLK_TCK))) * 1000);
		snprintf(bsize, sizeof(bsize), "%lu", vsize >> 10);
		snprintf(priori, sizeof(priori), "%u", (unsigned)round((priority + 100) * 99 / 139));

		process_entry *pentry = (process_entry *)calloc(1, sizeof(process_entry));
		if (!pentry) {
			BBF_ERR("failed to allocate memory for process entry");
			return;
		}

		list_add_tail(&pentry->list, &g_process_ctx.list);
		cur_process_num++;

		DM_STRNCPY(pentry->pid, entry->d_name, sizeof(pentry->pid));
		DM_STRNCPY(pentry->command, command, sizeof(pentry->command));
		DM_STRNCPY(pentry->size, bsize, sizeof(pentry->size));
		DM_STRNCPY(pentry->priority, priori, sizeof(pentry->priority));
		DM_STRNCPY(pentry->cputime, cputime, sizeof(pentry->cputime));
		DM_STRNCPY(pentry->state, get_proc_state(state), sizeof(pentry->state));
	}

	closedir(dir);

	int diff = cur_process_num - g_process_ctx.process_num;
	if (diff) {
		broadcast_add_del_event(diff);
		g_process_ctx.process_num = cur_process_num;
	}
}

static void free_process_list(void)
{
	process_entry *entry = NULL, *tmp = NULL;

	BBF_INFO("Free process list");

	list_for_each_entry_safe(entry, tmp, &g_process_ctx.list, list) {
		list_del(&entry->list);
		FREE(entry);
	}
}

static int get_instance_refresh_interval(void)
{
	char buf[8] = {0};

	sysmngr_uci_get("sysmngr", "process", "instance_refresh_interval", "0", buf, sizeof(buf));

	return (int)strtol(buf, NULL, 10);
}

static void run_refresh_process_list(void)
{
	free_process_list();
	init_process_list();

	if (g_process_ctx.refresh_interval > 0) {
		BBF_INFO("Scheduling process list update after %d sec...", g_process_ctx.refresh_interval);
		uloop_timeout_set(&g_process_ctx.instance_timer, g_process_ctx.refresh_interval * 1000);
	}
}

static void ubus_call_complete_cb(struct ubus_request *req, int ret)
{
	BBF_DEBUG("'tr069' ubus callback completed");
	run_refresh_process_list();
	FREE(req);
}

static void process_refresh_instance_timer(struct uloop_timeout *timeout)
{
	struct blob_buf bb = {0};

	memset(&bb, 0, sizeof(struct blob_buf));

	blob_buf_init(&bb, 0);
	int res = sysmngr_ubus_invoke_async(g_process_ctx.ubus_ctx, "tr069", "status", bb.head, NULL, ubus_call_complete_cb);
	blob_buf_free(&bb);

	if (res) {
		BBF_DEBUG("Update process list: 'tr069' ubus object not found");
		run_refresh_process_list();
	} else {
		BBF_DEBUG("Process list will be updated after 'tr069' ubus session completes");
	}
}

static void send_cpu_critical_state_event(unsigned int cpu_utilization)
{
	struct blob_buf bb = {0};
	char buf[32] = {0};

	snprintf(buf, sizeof(buf), "%u", cpu_utilization);

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	blobmsg_add_string(&bb, "name", "Device.DeviceInfo.ProcessStatus.CPU.1.CPUCriticalState!");

	void *arr = blobmsg_open_array(&bb, "input");

	void *cpu_table = blobmsg_open_table(&bb, NULL);
	blobmsg_add_string(&bb, "path", "CPUUtilization");
	blobmsg_add_string(&bb, "data", buf);
	blobmsg_add_string(&bb, "type", DMT_TYPE[DMT_UNINT]);
	blobmsg_close_table(&bb, cpu_table);

	void *name_table = blobmsg_open_table(&bb, NULL);
	blobmsg_add_string(&bb, "path", "Name");
	blobmsg_add_string(&bb, "data", DEFAULT_CPU_NAME);
	blobmsg_add_string(&bb, "type", DMT_TYPE[DMT_STRING]);
	blobmsg_close_table(&bb, name_table);

	blobmsg_close_array(&bb, arr);

	if (sysmngr_ubus_invoke_sync("bbfdm", "notify_event", bb.head, NULL, NULL)) {
		BBF_ERR("Failed to send 'CPUCriticalState!' event");
	} else {
		BBF_DEBUG("'CPUCriticalState!' event sent successfully with utilization at %u%%.", cpu_utilization);
	}

	blob_buf_free(&bb);
}

static unsigned int calculate_average_samples(unsigned int *samples)
{
	unsigned int num_samples = g_cpu_info.full_samples_reached ? g_cpu_info.num_samples : g_cpu_info.sample_index;
	unsigned int sum = 0;

	for (size_t i = 0; i < num_samples; i++) {
		sum += samples[i];
	}

	return num_samples ? (sum / num_samples) : 0;
}

static void run_cpu_monitor(void)
{
	char buf[32] = {0};

	jiffy_counts_t prev_jiffy = {
		.total_time = g_cpu_info.jiffy.total_time,
		.idle_time = g_cpu_info.jiffy.idle_time,
		.sys_time = g_cpu_info.jiffy.sys_time,
		.busy_time = g_cpu_info.jiffy.busy_time,
		.usr = g_cpu_info.jiffy.usr
	};

	get_jif_val(&g_cpu_info.jiffy);

	unsigned long long total_diff = g_cpu_info.jiffy.total_time - prev_jiffy.total_time;

	if (total_diff == 0)
		total_diff = 1;

	g_cpu_info.user_utilization_samples[g_cpu_info.sample_index] = ((g_cpu_info.jiffy.usr - prev_jiffy.usr) * 100) / total_diff;
	g_cpu_info.system_utilization_samples[g_cpu_info.sample_index] = ((g_cpu_info.jiffy.sys_time - prev_jiffy.sys_time) * 100) / total_diff;
	g_cpu_info.idle_utilization_samples[g_cpu_info.sample_index] = ((g_cpu_info.jiffy.idle_time - prev_jiffy.idle_time) * 100) / total_diff;
	g_cpu_info.utilization_samples[g_cpu_info.sample_index] = ((g_cpu_info.jiffy.busy_time - prev_jiffy.busy_time) * 100) / total_diff;

	if (!g_cpu_info.full_samples_reached) {
	    g_cpu_info.full_samples_reached = ((g_cpu_info.sample_index + 1) >= g_cpu_info.num_samples);
	}

	g_cpu_info.sample_index = (g_cpu_info.sample_index + 1) % g_cpu_info.num_samples;

	unsigned int avg_utilization = calculate_average_samples(g_cpu_info.utilization_samples);

	if ((avg_utilization > g_cpu_info.critical_rise_threshold) &&
		(g_cpu_info.critical_fall_time >= g_cpu_info.critical_rise_time)) {

		BBF_ERR("CPU utilization reached critical threshold: %u%% !!!!!!!!", avg_utilization);

		// Update CriticalRiseTimeStamp to the current time
		g_cpu_info.critical_rise_time = time(NULL);
		snprintf(buf, sizeof(buf), "%ld", (long int)g_cpu_info.critical_rise_time);
		sysmngr_uci_set("sysmngr", "cpu", "critical_rise_time", buf);

		if (g_cpu_info.enable_critical_log) {
			// Generate log into the vendor log file referenced by 'VendorLogFileRef' parameter indicating critical condition is reached
			sysmngr_generate_critical_log_file(g_cpu_info.log_file, "CPU", true);
		}

		// Send 'CPUCriticalState!' event
		send_cpu_critical_state_event(avg_utilization);
	}

	if ((avg_utilization < g_cpu_info.critical_fall_threshold) &&
		(g_cpu_info.critical_rise_time > g_cpu_info.critical_fall_time)) {

		BBF_ERR("CPU utilization has fallen below critical threshold: %u%% !!!!!!!!", avg_utilization);

		// Update CriticalFallTimeStamp to the current time
		g_cpu_info.critical_fall_time = time(NULL);
		snprintf(buf, sizeof(buf), "%ld", (long int)g_cpu_info.critical_fall_time);
		sysmngr_uci_set("sysmngr", "cpu", "critical_fall_time", buf);

		if (g_cpu_info.enable_critical_log) {
			// Generate log into the vendor log file referenced by 'VendorLogFileRef' parameter indicating that the critical condition is no longer present
			sysmngr_generate_critical_log_file(g_cpu_info.log_file, "CPU", false);
		}
	}

	BBF_INFO("Next memory monitor check scheduled in %d sec...", g_cpu_info.poll_interval);
	uloop_timeout_set(&g_cpu_info.cpu_timer, g_cpu_info.poll_interval * 1000);
}

static void cpu_timer_callback(struct uloop_timeout *timeout)
{
	run_cpu_monitor();
}

static int fill_global_cpu_info(void)
{
	char buf[16] = {0};

	memset(&g_cpu_info, 0, sizeof(struct cpu_info));

	g_cpu_info.cpu_timer.cb = cpu_timer_callback;

	sysmngr_uci_get("sysmngr", "cpu", "enable", "0", buf, sizeof(buf));
	g_cpu_info.enable = ((int)strtol(buf, NULL, 10) != 0);
	BBF_DEBUG("Memory Monitor Config: |Enable| |%d|", g_cpu_info.enable);

	sysmngr_uci_get("sysmngr", "cpu", "enable_critical_log", "0", buf, sizeof(buf));
	g_cpu_info.enable_critical_log = ((int)strtol(buf, NULL, 10) != 0);
	BBF_DEBUG("Memory Monitor Config: |EnableCriticalLog| |%d|", g_cpu_info.enable_critical_log);

	sysmngr_uci_get("sysmngr", "cpu", "poll_interval", DEFAULT_CPU_POLL_INTERVAL, buf, sizeof(buf));
	g_cpu_info.poll_interval = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |PollInterval| |%lu|", g_cpu_info.poll_interval);

	sysmngr_uci_get("sysmngr", "cpu", "num_samples", DEFAULT_CPU_NUM_SAMPLES, buf, sizeof(buf));
	g_cpu_info.num_samples = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |NumSamples| |%lu|", g_cpu_info.num_samples);

	sysmngr_uci_get("sysmngr", "cpu", "critical_rise_threshold", DEFAULT_CPU_CRITICAL_RISE_THRESHOLD, buf, sizeof(buf));
	g_cpu_info.critical_rise_threshold = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalRiseThreshold| |%lu|", g_cpu_info.critical_rise_threshold);

	sysmngr_uci_get("sysmngr", "cpu", "critical_fall_threshold", DEFAULT_CPU_CRITICAL_FALL_THRESHOLD, buf, sizeof(buf));
	g_cpu_info.critical_fall_threshold = strtoul(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalFallThreshold| |%lu|", g_cpu_info.critical_fall_threshold);

	sysmngr_uci_get("sysmngr", "cpu", "critical_rise_time", "0", buf, sizeof(buf));
	g_cpu_info.critical_rise_time = strtol(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalRiseTimeStamp| |%lu|", g_cpu_info.critical_rise_time);

	sysmngr_uci_get("sysmngr", "cpu", "critical_fall_time", "0", buf, sizeof(buf));
	g_cpu_info.critical_fall_time = strtol(buf, NULL, 10);
	BBF_DEBUG("Memory Monitor Config: |CriticalFallTimeStamp| |%lu|", g_cpu_info.critical_fall_time);

	sysmngr_uci_get("sysmngr", "cpu", "file_path", DEFAULT_CPU_CRITICAL_LOG_PATH, g_cpu_info.log_file, sizeof(g_cpu_info.log_file));
	BBF_DEBUG("Memory Monitor Config: |FilePath| |%s|", g_cpu_info.log_file);
	if (!file_exists(g_cpu_info.log_file)) {
		// Create empty file if it doesn't exist
		create_empty_file(g_cpu_info.log_file);
	}

	g_cpu_info.utilization_samples = calloc(g_cpu_info.num_samples, sizeof(unsigned int));
	g_cpu_info.user_utilization_samples = calloc(g_cpu_info.num_samples, sizeof(unsigned int));
	g_cpu_info.system_utilization_samples = calloc(g_cpu_info.num_samples, sizeof(unsigned int));
	g_cpu_info.idle_utilization_samples = calloc(g_cpu_info.num_samples, sizeof(unsigned int));
	if (!g_cpu_info.utilization_samples || !g_cpu_info.user_utilization_samples ||
		!g_cpu_info.system_utilization_samples || !g_cpu_info.idle_utilization_samples) {
		BBF_ERR("Failed to allocate memory for mode utilization samples");
		return -1;
	}

	get_jif_val(&g_cpu_info.jiffy);
	return 0;
}

static void free_global_cpu_info(void)
{
	FREE(g_cpu_info.utilization_samples);
	FREE(g_cpu_info.user_utilization_samples);
	FREE(g_cpu_info.system_utilization_samples);
	FREE(g_cpu_info.idle_utilization_samples);
}

/*************************************************************
* EXTERNAL APIS
**************************************************************/
void sysmngr_process_init(struct ubus_context *ubus_ctx)
{
	g_process_ctx.ubus_ctx = ubus_ctx;
	g_process_ctx.refresh_interval = get_instance_refresh_interval();
	g_process_ctx.instance_timer.cb = process_refresh_instance_timer;
	INIT_LIST_HEAD(&g_process_ctx.list);
	g_process_ctx.process_num = 0;

	run_refresh_process_list();
}

void sysmngr_process_clean(struct ubus_context *ubus_ctx)
{
	free_process_list();
	uloop_timeout_cancel(&g_process_ctx.instance_timer);
}

void sysmngr_cpu_init(void)
{
	int res = fill_global_cpu_info();
	if (res) {
		BBF_ERR("Can't start CPU monitoring!!");
		return;
	}

	if (!g_cpu_info.enable) {
		BBF_INFO("CPU monitoring is disabled.");
		return;
	} else {
		BBF_INFO("CPU monitoring is enabled");
	}

	BBF_INFO("Next CPU monitor check scheduled in %d sec...", g_cpu_info.poll_interval);
	uloop_timeout_set(&g_cpu_info.cpu_timer, g_cpu_info.poll_interval * 1000);
}

void sysmngr_cpu_clean(void)
{
	free_global_cpu_info();
	uloop_timeout_cancel(&g_cpu_info.cpu_timer);
	BBF_INFO("CPU monitoring process stopped");
}

/*************************************************************
* ENTRY METHOD
**************************************************************/
static int browseProcessEntriesInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct process_entry *entry = NULL;
	struct dm_data curr_data = {0};
	char *inst = NULL;
	int id = 0;

	if (g_process_ctx.refresh_interval <= 0) {
		BBF_INFO("Scheduling process list update after 2 sec...");
		uloop_timeout_set(&g_process_ctx.instance_timer, 2 * 1000);
	}

	list_for_each_entry(entry, &g_process_ctx.list, list) {

		curr_data.additional_data = entry;

		inst = handle_instance_without_section(dmctx, parent_node, ++id);

		if (DM_LINK_INST_OBJ(dmctx, parent_node, &curr_data, inst) == DM_STOP)
			break;
	}

	return 0;
}

static int browseDeviceInfoProcessStatusCPUInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance)
{
	struct dm_data data = {0};

	struct uci_section *s = is_dmmap_section_exist("dmmap_sysmngr", "cpu");
	if (!s) dmuci_add_section_bbfdm("dmmap_sysmngr", "cpu", &s);

	data.dmmap_section = s;

	handle_instance(dmctx, parent_node, s, "cpu_instance", "cpu_alias");
	DM_LINK_INST_OBJ(dmctx, parent_node, &data, "1");
	return 0;


}

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_process_cpu_usage(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *cpu_monitor_enable = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "enable", "0");

	if (DM_STRCMP(cpu_monitor_enable, "1") == 0) {
		dmasprintf(value, "%u", calculate_average_samples(g_cpu_info.utilization_samples));
	} else {
		dmasprintf(value, "%u", get_cpu_usage());
	}

	return 0;
}

static int get_process_number_of_entries(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseProcessEntriesInst);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_DeviceInfoProcessStatus_CPUNumberOfEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseDeviceInfoProcessStatusCPUInst);
	dmasprintf(value, "%d", cnt);
	return 0;
}

static int get_process_pid(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->pid : "";
	return 0;
}

static int get_process_command(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->command : "";
	return 0;
}

static int get_process_size(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->size : "";
	return 0;
}

static int get_process_priority(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->priority : "";
	return 0;
}

static int get_process_cpu_time(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->cputime : "";
	return 0;
}

static int get_process_state(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = data ? ((struct process_entry *)((struct dm_data *)data)->additional_data)->state : "";
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	return bbf_get_alias(ctx, ((struct dm_data *)data)->dmmap_section, "cpu_alias", instance, value);
}

static int set_DeviceInfoProcessStatusCPU_Alias(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	return bbf_set_alias(ctx, ((struct dm_data *)data)->dmmap_section, "cpu_alias", instance, value);
}

static int get_DeviceInfoProcessStatusCPU_Name(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmstrdup(DEFAULT_CPU_NAME);
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_Enable(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "enable", "0");
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_Enable(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	bool b;

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_boolean(ctx, value))
				return FAULT_9007;
			break;
		case VALUESET:
			string_to_bool(value, &b);
			dmuci_set_value("sysmngr", "cpu", "enable", b ? "1" : "0");
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_UpTime(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%d", sysmngr_get_uptime());
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_UserModeUtilization(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", calculate_average_samples(g_cpu_info.user_utilization_samples));
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_SystemModeUtilization(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", calculate_average_samples(g_cpu_info.system_utilization_samples));
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_IdleModeUtilization(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", calculate_average_samples(g_cpu_info.idle_utilization_samples));
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_CPUUtilization(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", calculate_average_samples(g_cpu_info.utilization_samples));
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_PollInterval(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "poll_interval", DEFAULT_CPU_POLL_INTERVAL);
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_PollInterval(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,NULL}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "cpu", "poll_interval", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_NumSamples(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "num_samples", DEFAULT_CPU_NUM_SAMPLES);
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_NumSamples(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{"1","300"}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "cpu", "num_samples", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_CriticalRiseThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "critical_rise_threshold", DEFAULT_CPU_CRITICAL_RISE_THRESHOLD);
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_CriticalRiseThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,"100"}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "cpu", "critical_rise_threshold", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_CriticalFallThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "critical_fall_threshold", DEFAULT_CPU_CRITICAL_FALL_THRESHOLD);
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_CriticalFallThreshold(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_unsignedInt(ctx, value, RANGE_ARGS{{NULL,"100"}}, 1))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("sysmngr", "cpu", "critical_fall_threshold", value);
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_CriticalRiseTimeStamp(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *rise_time = NULL;

	dmuci_get_option_value_string("sysmngr", "cpu", "critical_rise_time", &rise_time);

	return dm_time_utc_format(DM_STRTOL(rise_time), value);
}

static int get_DeviceInfoProcessStatusCPU_CriticalFallTimeStamp(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *fall_time = NULL;

	dmuci_get_option_value_string("sysmngr", "cpu", "critical_fall_time", &fall_time);

	return dm_time_utc_format(DM_STRTOL(fall_time), value);
}

static int get_DeviceInfoProcessStatusCPU_EnableCriticalLog(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "enable_critical_log", "0");
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_EnableCriticalLog(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	bool b;

	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_boolean(ctx, value))
				return FAULT_9007;
			break;
		case VALUESET:
			string_to_bool(value, &b);
			dmuci_set_value("sysmngr", "cpu", "enable_critical_log", b ? "1" : "0");
			break;
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_VendorLogFileRef(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *file_path = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "file_path", DEFAULT_CPU_CRITICAL_LOG_PATH);

	if (file_exists(file_path)) {
		char file_uri[512] = {0};

		// if there is a path, then prepend file:// to it to comply with bbf requirement of file URI
		snprintf(file_uri, sizeof(file_uri), "file://%s", file_path);

		// get the vendor file path
		_bbfdm_get_references(ctx, "Device.DeviceInfo.VendorLogFile.", "Name", file_uri, value);
	}
	return 0;
}

static int get_DeviceInfoProcessStatusCPU_FilePath(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "file_path", DEFAULT_CPU_CRITICAL_LOG_PATH);
	return 0;
}

static int set_DeviceInfoProcessStatusCPU_FilePath(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	char *file_path = dmuci_get_option_value_fallback_def("sysmngr", "cpu", "file_path", DEFAULT_CPU_CRITICAL_LOG_PATH);

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

			dmuci_set_value("sysmngr", "cpu", "file_path", value);
			break;
	}
	return 0;
}

/*************************************************************
 * EVENTS
 *************************************************************/
static event_args CPUCriticalState_event_args = {
	.name = "", // This field is left empty because we are not listening to any external events, The system now operates within a single unified daemon,
				// removing the need for separate event listeners. See send_cpu_critical_state_event API for details on implementation.
	.param = (const char *[]) {
		"CPUUtilization",
		"Name",
		NULL
	}
};

static int get_event_CPUCriticalState(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = (char *)&CPUCriticalState_event_args;
	return 0;
}

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.ProcessStatus.Process.{i}. *** */
DMLEAF tDeviceInfoProcessStatusProcessParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"PID", &DMREAD, DMT_UNINT, get_process_pid, NULL, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Command", &DMREAD, DMT_STRING, get_process_command, NULL, BBFDM_BOTH},
{"Size", &DMREAD, DMT_UNINT, get_process_size, NULL, BBFDM_BOTH},
{"Priority", &DMREAD, DMT_UNINT, get_process_priority, NULL, BBFDM_BOTH},
{"CPUTime", &DMREAD, DMT_UNINT, get_process_cpu_time, NULL, BBFDM_BOTH},
{"State", &DMREAD, DMT_STRING, get_process_state, NULL, BBFDM_BOTH},
{0}
};

/* *** Device.DeviceInfo.ProcessStatus.CPU.{i}. *** */
DMLEAF tDeviceInfoProcessStatusCPUParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"Alias", &DMWRITE, DMT_STRING, get_DeviceInfoProcessStatusCPU_Alias, set_DeviceInfoProcessStatusCPU_Alias, BBFDM_BOTH},
{"Name", &DMREAD, DMT_STRING, get_DeviceInfoProcessStatusCPU_Name, NULL, BBFDM_BOTH, DM_FLAG_UNIQUE|DM_FLAG_LINKER},
{"Enable", &DMWRITE, DMT_BOOL, get_DeviceInfoProcessStatusCPU_Enable, set_DeviceInfoProcessStatusCPU_Enable, BBFDM_BOTH},
{"UpTime", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatusCPU_UpTime, NULL, BBFDM_BOTH},
{"UserModeUtilization", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatusCPU_UserModeUtilization, NULL, BBFDM_BOTH},
{"SystemModeUtilization", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatusCPU_SystemModeUtilization, NULL, BBFDM_BOTH},
{"IdleModeUtilization", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatusCPU_IdleModeUtilization, NULL, BBFDM_BOTH},
{"CPUUtilization", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatusCPU_CPUUtilization, NULL, BBFDM_BOTH},
{"PollInterval", &DMWRITE, DMT_UNINT, get_DeviceInfoProcessStatusCPU_PollInterval, set_DeviceInfoProcessStatusCPU_PollInterval, BBFDM_BOTH},
{"NumSamples", &DMWRITE, DMT_UNINT, get_DeviceInfoProcessStatusCPU_NumSamples, set_DeviceInfoProcessStatusCPU_NumSamples, BBFDM_BOTH},
{"CriticalRiseThreshold", &DMWRITE, DMT_UNINT, get_DeviceInfoProcessStatusCPU_CriticalRiseThreshold, set_DeviceInfoProcessStatusCPU_CriticalRiseThreshold, BBFDM_BOTH},
{"CriticalFallThreshold", &DMWRITE, DMT_UNINT, get_DeviceInfoProcessStatusCPU_CriticalFallThreshold, set_DeviceInfoProcessStatusCPU_CriticalFallThreshold, BBFDM_BOTH},
{"CriticalRiseTimeStamp", &DMREAD, DMT_TIME, get_DeviceInfoProcessStatusCPU_CriticalRiseTimeStamp, NULL, BBFDM_BOTH},
{"CriticalFallTimeStamp", &DMREAD, DMT_TIME, get_DeviceInfoProcessStatusCPU_CriticalFallTimeStamp, NULL, BBFDM_BOTH},
{"EnableCriticalLog", &DMWRITE, DMT_BOOL, get_DeviceInfoProcessStatusCPU_EnableCriticalLog, set_DeviceInfoProcessStatusCPU_EnableCriticalLog, BBFDM_BOTH},
{"VendorLogFileRef", &DMREAD, DMT_STRING, get_DeviceInfoProcessStatusCPU_VendorLogFileRef, NULL, BBFDM_BOTH},
{"FilePath", &DMWRITE, DMT_STRING, get_DeviceInfoProcessStatusCPU_FilePath, set_DeviceInfoProcessStatusCPU_FilePath, BBFDM_BOTH},
{"CPUCriticalState!", &DMREAD, DMT_EVENT, get_event_CPUCriticalState, NULL, BBFDM_USP},
{0}
};

/* *** Device.DeviceInfo.ProcessStatus. *** */
DMOBJ tDeviceInfoProcessStatusObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type, uniqueKeys */
{"Process", &DMREAD, NULL, NULL, NULL, browseProcessEntriesInst, NULL, NULL, NULL, tDeviceInfoProcessStatusProcessParams, NULL, BBFDM_BOTH, NULL},
{"CPU", &DMREAD, NULL, NULL, NULL, browseDeviceInfoProcessStatusCPUInst, NULL, NULL, NULL, tDeviceInfoProcessStatusCPUParams, NULL, BBFDM_BOTH, NULL},
{0}
};

DMLEAF tDeviceInfoProcessStatusParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type */
{"CPUUsage", &DMREAD, DMT_UNINT, get_process_cpu_usage, NULL, BBFDM_BOTH},
{"ProcessNumberOfEntries", &DMREAD, DMT_UNINT, get_process_number_of_entries, NULL, BBFDM_BOTH},
{"CPUNumberOfEntries", &DMREAD, DMT_UNINT, get_DeviceInfoProcessStatus_CPUNumberOfEntries, NULL, BBFDM_BOTH},
{0}
};
