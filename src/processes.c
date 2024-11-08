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
	unsigned long long total;
	unsigned long long busy;
} jiffy_counts_t;

typedef struct process_ctx {
	struct ubus_context *ubus_ctx;
	struct uloop_timeout instance_timer;
	struct list_head list;
	int refresh_interval;
	int process_num;
} process_ctx;

static process_ctx g_process_ctx = {0};

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
				p_jif->total = p_jif->usr + p_jif->nic + p_jif->sys + p_jif->idle
					+ p_jif->iowait + p_jif->irq + p_jif->softirq + p_jif->steal;

				p_jif->busy = p_jif->total - p_jif->idle - p_jif->iowait;
				break;
			}
		}
		fclose(file);
	}
}

static unsigned int get_cpu_load(jiffy_counts_t *prev_jif, jiffy_counts_t *cur_jif)
{
	unsigned total_diff, cpu;

	total_diff = (unsigned)(cur_jif->total - prev_jif->total);

	if (total_diff == 0)
		total_diff = 1;

	cpu = 100 * (unsigned)(cur_jif->busy - prev_jif->busy) / total_diff;

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

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_process_cpu_usage(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmasprintf(value, "%u", get_cpu_usage());
	return 0;
}

static int get_process_number_of_entries(char* refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	int cnt = get_number_of_entries(ctx, data, instance, browseProcessEntriesInst);
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

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo.ProcessStatus.Process.{i}. *** */
DMLEAF tDeviceInfoProcessStatusProcessParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"PID", &DMREAD, DMT_UNINT, get_process_pid, NULL, BBFDM_BOTH, DM_FLAG_UNIQUE},
{"Command", &DMREAD, DMT_STRING, get_process_command, NULL, BBFDM_BOTH},
{"Size", &DMREAD, DMT_UNINT, get_process_size, NULL, BBFDM_BOTH},
{"Priority", &DMREAD, DMT_UNINT, get_process_priority, NULL, BBFDM_BOTH},
{"CPUTime", &DMREAD, DMT_UNINT, get_process_cpu_time, NULL, BBFDM_BOTH},
{"State", &DMREAD, DMT_STRING, get_process_state, NULL, BBFDM_BOTH},
{0}
};

/* *** Device.DeviceInfo.ProcessStatus. *** */
DMOBJ tDeviceInfoProcessStatusObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type, uniqueKeys, version*/
{"Process", &DMREAD, NULL, NULL, NULL, browseProcessEntriesInst, NULL, NULL, NULL, tDeviceInfoProcessStatusProcessParams, NULL, BBFDM_BOTH, NULL},
{0}
};

DMLEAF tDeviceInfoProcessStatusParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type, version*/
{"CPUUsage", &DMREAD, DMT_UNINT, get_process_cpu_usage, NULL, BBFDM_BOTH},
{"ProcessNumberOfEntries", &DMREAD, DMT_UNINT, get_process_number_of_entries, NULL, BBFDM_BOTH},
{0}
};
