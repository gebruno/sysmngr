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

extern struct list_head global_memhead;

LIST_HEAD(process_list);
static int process_count = 0;

#define PROCPS_BUFSIZE 1024

struct process_entry {
	struct list_head list;

	char command[256];
	char state[16];
	char pid[8];
	char size[8];
	char priority[8];
	char cputime[8];
};

typedef struct jiffy_counts_t {
	unsigned long long usr, nic, sys, idle;
	unsigned long long iowait, irq, softirq, steal;
	unsigned long long total;
	unsigned long long busy;
} jiffy_counts_t;

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

static bool is_update_process_allowed(void)
{
	char *tr069_status = NULL;

	if (dmubus_object_method_exists("tr069")) {
		struct uci_section *s = NULL, *stmp = NULL;
		uci_path_foreach_sections_safe(varstate, "icwmp", "sess_status", stmp, s) {
			dmuci_get_value_by_section_string(s, "current_status", &tr069_status);
		}
	}

	if (tr069_status == NULL)
		goto end;

	if (strcmp(tr069_status, "running") == 0) {
		return false;
	}

end:
	return true;
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

static struct process_entry *check_entry_exists(const char *pid)
{
	struct process_entry *entry = NULL;

	list_for_each_entry(entry, &process_list, list) {
		if (DM_STRCMP(entry->pid, pid) == 0)
			return entry;
	}

	return NULL;
}

static void check_killed_process(void)
{
	struct process_entry *entry = NULL;
	struct process_entry *entry_tmp = NULL;
	char fstat[32];

	list_for_each_entry_safe(entry, entry_tmp, &process_list, list) {

		snprintf(fstat, sizeof(fstat), "/proc/%s/stat", entry->pid);
		if (file_exists(fstat))
			continue;

		list_del(&entry->list);
		dmfree(entry);
	}
}

static void procps_get_cmdline(char *buf, int bufsz, const char *pid, const char *comm)
{
	int sz;
	char filename[270];

	snprintf(filename, sizeof(filename), "/proc/%s/cmdline", pid);
	sz = dm_file_to_buf(filename, buf, bufsz);
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

static void init_processes(void)
{
	DIR *dir = NULL;
	struct dirent *entry = NULL;
	struct stat stats = {0};
	char buf[PROCPS_BUFSIZE];
	char fstat[288];
	char command[256];
	char comm[32];
	char bsize[32];
	char cputime[32];
	char priori[32];
	char *comm1 = NULL;
	char *comm2 = NULL;
	char state;
	unsigned long stime;
	unsigned long utime;
	unsigned long vsize;
	int priority, n;
	int curr_process_idx = 0;

	if (!is_update_process_allowed())
		return;

	check_killed_process();

	dir = opendir("/proc");
	if (dir == NULL)
		return;

	while ((entry = readdir(dir)) != NULL) {
		struct process_entry *pentry = NULL;
		struct process_entry *pentry_exits = NULL;

		int digit = entry->d_name[0] - '0';
		if (digit < 0 || digit > 9)
			continue;

		snprintf(fstat, sizeof(fstat), "/proc/%s/stat", entry->d_name);
		if (stat(fstat, &stats))
			continue;

		n = dm_file_to_buf(fstat, buf, PROCPS_BUFSIZE);
		if (n < 0)
			continue;

		comm2 = strrchr(buf, ')'); /* split into "PID (cmd" and "<rest>" */
		if (!comm2) /* sanity check */
		  continue;

		comm2[0] = '\0';
		comm1 = strchr(buf, '(');
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
		curr_process_idx++;

		snprintf(cputime, sizeof(cputime), "%lu", ((stime / sysconf(_SC_CLK_TCK)) + (utime / sysconf(_SC_CLK_TCK))) * 1000);
		snprintf(bsize, sizeof(bsize), "%lu", vsize >> 10);
		snprintf(priori, sizeof(priori), "%u", (unsigned)round((priority + 100) * 99 / 139));

		if (process_count == 0 || !(pentry_exits = check_entry_exists(entry->d_name))) {

			pentry = dm_dynamic_calloc(&global_memhead, 1, sizeof(struct process_entry));
			if (!pentry)
				return;

			list_add_tail(&pentry->list, &process_list);
		}

		if (pentry_exits)
			pentry = pentry_exits;

		DM_STRNCPY(pentry->pid, entry->d_name, sizeof(pentry->pid));
		DM_STRNCPY(pentry->command, command, sizeof(pentry->command));
		DM_STRNCPY(pentry->size, bsize, sizeof(pentry->size));
		DM_STRNCPY(pentry->priority, priori, sizeof(pentry->priority));
		DM_STRNCPY(pentry->cputime, cputime, sizeof(pentry->cputime));
		DM_STRNCPY(pentry->state, get_proc_state(state), sizeof(pentry->state));
	}

	closedir(dir);
	process_count = curr_process_idx;
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

	init_processes();
	list_for_each_entry(entry, &process_list, list) {

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



