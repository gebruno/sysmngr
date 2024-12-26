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
#include "fwbank.h"

#ifdef SYSMNGR_FWBANK_UBUS_SUPPORT
const char fw_image_dependency[] = "file:/etc/sysmngr/fwbank";
#define FWBANK_FILE_PATH "/etc/sysmngr/fwbank"
#else
const char fw_image_dependency[] = "file:/usr/libexec/rpcd/fwbank";
#define FWBANK_FILE_PATH "/usr/libexec/rpcd/fwbank"
#endif

#define FWBANK_DUMP_CMD FWBANK_FILE_PATH " call dump"
#define FWBANK_SET_BOOTBANK_CMD FWBANK_FILE_PATH " call set_bootbank"
#define FWBANK_UPGRADE_CMD FWBANK_FILE_PATH " call upgrade"

#define FWBANK_DUMP_MAX_RETRIES 10
#define FWBANK_DUMP_RETRY_DELAY 3

struct fwbank_event_data {
	struct ubus_context *ctx;
	struct uloop_timeout tm;
	struct ubus_event_handler ev;
	uint32_t band_id;
	int timeout;
};

struct fwbank_dump_data {
	struct ubus_context *ctx;
	struct uloop_timeout tm;
	struct blob_buf output;
};

static struct fwbank_dump_data g_fwbank_dump = {0};
static int g_retry_count = 0;

typedef void (*sysmngr_task_callback_t)(struct ubus_context *ctx, struct ubus_request_data *req, int *pipe_fds, uint32_t bank_id);

typedef struct sysmngr_task_data {
	struct ubus_context *ctx;
	struct ubus_request_data *req;
	struct ubus_request_data new_req;
	struct uloop_process process; // Used for forked task
	struct uloop_timeout timeoutcb; // Timeout for the task
	sysmngr_task_callback_t finishcb; // Finish callback for parent process
	const char *command; // Command to execute
	uint32_t bank_id;
	int pipe_fds[2];
} sysmngr_task_data_t;

struct blobmsg_policy sysmngr_dump_policy[] = {
	{ "bank", BLOBMSG_TYPE_ARRAY }
};

struct blobmsg_policy sysmngr_bank_policy[] = {
	{ "name", BLOBMSG_TYPE_STRING },
	{ "id", BLOBMSG_TYPE_INT32 },
	{ "active", BLOBMSG_TYPE_BOOL },
	{ "boot", BLOBMSG_TYPE_BOOL },
	{ "upgrade", BLOBMSG_TYPE_BOOL },
	{ "fwver", BLOBMSG_TYPE_STRING },
	{ "swver", BLOBMSG_TYPE_STRING },
	{ "omci_swver", BLOBMSG_TYPE_STRING },
	{ "status", BLOBMSG_TYPE_STRING }
};

static void _sysmngr_task_finish_callback(struct uloop_process *p, int ret)
{
	struct sysmngr_task_data *task = NULL;

	task = container_of(p, struct sysmngr_task_data, process);

	if (task == NULL) {
		BBF_ERR("Failed to decode forked task");
		return;
	}

	if (task->timeoutcb.pending) {
		// Cancel timeout if the task finishes on its own
		uloop_timeout_cancel(&task->timeoutcb);
	}

	if (task->finishcb) {
		task->finishcb(task->ctx, &task->new_req, task->pipe_fds, task->bank_id);
	}

	if (task->ctx && task->req) {
		ubus_complete_deferred_request(task->ctx, &task->new_req, 0);
	}

	FREE(task);
}

static void task_callback(const char *command, int *pipe_fds)
{
	// Redirect stdout to the pipe's write end
	dup2(pipe_fds[1], STDOUT_FILENO);
	close(pipe_fds[0]); // Close unused read end
	close(pipe_fds[1]); // Close write end after dup2

	if (!command)
		exit(EXIT_FAILURE);

	char *line = NULL;
	ssize_t read;
	size_t len;

	FILE *pipe = popen(command, "r"); // flawfinder: ignore
	if (pipe == NULL)
		exit(EXIT_FAILURE);

	while ((read = getline(&line, &len, pipe)) != -1) {
		// Output the command's result to stdout
		line[read - 1] = '\0'; // Remove trailing newline
		printf("%s", line);
	}

	free(line);

	if (pclose(pipe) == -1)
		exit(EXIT_FAILURE);

	exit(EXIT_SUCCESS);
}

static void timeout_callback(struct uloop_timeout *t)
{
	sysmngr_task_data_t *task = container_of(t, sysmngr_task_data_t, timeoutcb);

	if (task && task->process.pid > 0) {
		BBF_ERR("Task timed out. Killing process with PID %d\n", task->process.pid);
		kill(task->process.pid, SIGKILL);
	}
}

static int sysmngr_task_fork(sysmngr_task_callback_t finishcb, const char *command, int timeout, struct ubus_request_data *req, uint32_t bank_id)
{
	sysmngr_task_data_t *task;
	pid_t child;

	task = (sysmngr_task_data_t *)calloc(sizeof(sysmngr_task_data_t), 1);
	if (task == NULL) {
		return -1;
	}

	if (pipe(task->pipe_fds) == -1) {
		BBF_ERR("pipe failed");
		FREE(task);
		return -1;
	}

	task->command = command;
	task->ctx = g_fwbank_dump.ctx;
	task->req = req;
	task->bank_id = bank_id;

	child = fork();
	if (child == -1) {
		BBF_ERR("Failed to fork a child for task");
		FREE(task);
		return -1;
	} else if (child == 0) {
		/* free fd's and memory inherited from parent */
		uloop_done();
		fclose(stdin);
		fclose(stderr);

		task_callback(task->command, task->pipe_fds);

		/* write result and exit */
		exit(EXIT_SUCCESS);
	}

	if (finishcb) {
		task->finishcb = finishcb;
		task->process.pid = child;
		task->process.cb = _sysmngr_task_finish_callback;
		uloop_process_add(&task->process);
	}

	if (task->ctx && task->req) {
		ubus_defer_request(task->ctx, task->req, &task->new_req);
	}

	if (timeout > 0) {
		task->timeoutcb.cb = timeout_callback;
		uloop_timeout_set(&task->timeoutcb, 1000 * timeout);
	}

	return 0;
}

static void fwbank_dump_timer(struct uloop_timeout *timeout)
{
	struct fwbank_dump_data *data = NULL;

	BBF_DEBUG("fwbank_dump_timer triggered");

	data = container_of(timeout, struct fwbank_dump_data, tm);
	if (data == NULL)
		return;

	sysmngr_init_fwbank_dump(data->ctx);
}

static int free_global_fwbank_dump(struct blob_buf *fwbank_dump_bb)
{
	if (fwbank_dump_bb->head && blob_len(fwbank_dump_bb->head)) {
		BBF_DEBUG("Freeing fwbank dump blob buffer");
		blob_buf_free(fwbank_dump_bb);
	} else {
		BBF_DEBUG("fwbank dump blob buffer is already empty");
	}

	return 0;
}

static int validate_global_fwbank_dump(struct blob_buf *fwbank_dump_bb)
{
	if (!fwbank_dump_bb->head || !blob_len(fwbank_dump_bb->head)) {
		BBF_ERR("fwbank dump output is empty");
		return -1;
	}

	BBF_DEBUG("Validating global fwbank dump");
	struct blob_attr *tb[1] = {0};

	if (blobmsg_parse(sysmngr_dump_policy, 1, tb, blobmsg_data(fwbank_dump_bb->head), blobmsg_len(fwbank_dump_bb->head))) {
		BBF_ERR("Failed to parse fwbank dump blob");
		return -1;
	}

	if (!tb[0]) { // bank array is not found
		BBF_ERR("Bank array not found in fwbank dump");
		return -1;
	}

	struct blob_attr *entry = NULL;
	int rem = 0;

	blobmsg_for_each_attr(entry, tb[0], rem) { // parse bank array
		struct blob_attr *t[9] = {0};

		if (blobmsg_parse(sysmngr_bank_policy, 9, t, blobmsg_data(entry), blobmsg_len(entry))) {
			BBF_ERR("Failed to parse bank entry");
			return -1;
		}

		if (!t[0] || !t[1] || !t[2] || !t[3] || !t[4] || !t[5] || !t[6] || !t[7] || !t[8])
			return -1;
	}

	BBF_DEBUG("Global fwbank dump validation passed");
	return 0;
}

static void fwbank_dump_finish_callback(struct ubus_context *ctx, struct ubus_request_data *req, int *pipe_fds, uint32_t bank_id)
{
	BBF_DEBUG("Task finished Line=%d && func=%s", __LINE__, __func__);

	close(pipe_fds[1]); // Close unused write end

	char buffer[1024] = {0};
	ssize_t bytes_read;

	BBF_DEBUG("Reading script output...");

	// Read the output from the script
	while ((bytes_read = read(pipe_fds[0], buffer, sizeof(buffer) - 1)) > 0) {
		buffer[bytes_read] = '\0'; // Null-terminate the buffer
		BBF_DEBUG("Script output: %s", buffer);
	}

	close(pipe_fds[0]); // Close read end

	if (bytes_read < 0 || strlen(buffer) == 0) {
		BBF_ERR("Failed to read from pipe");
		goto retry;
	}

	// Use a temporary blob_buf for validation
	struct blob_buf temp_buf = {0};

	memset(&temp_buf, 0, sizeof(struct blob_buf));
	blob_buf_init(&temp_buf, 0);

	if (!blobmsg_add_json_from_string(&temp_buf, buffer)) {
	    BBF_ERR("Invalid JSON format in buffer");
	    blob_buf_free(&temp_buf);
	    goto retry;
	}

	int res = validate_global_fwbank_dump(&temp_buf);
	if (res) {
		BBF_ERR("Failed to validate 'fwbank' output");
		blob_buf_free(&temp_buf);
		goto retry;
	}

	free_global_fwbank_dump(&g_fwbank_dump.output);

	// Init g_fwbank_dump.output
	memset(&g_fwbank_dump.output, 0, sizeof(struct blob_buf));
	blob_buf_init(&g_fwbank_dump.output, 0);

	// Move validated JSON from temp_buf to g_fwbank_dump.output
	blobmsg_add_blob(&g_fwbank_dump.output, blob_data(temp_buf.head));

	// Free the temporary buffer
	blob_buf_free(&temp_buf);

	return;

retry:
	if (g_retry_count < FWBANK_DUMP_MAX_RETRIES) {
		g_retry_count++;
		uloop_timeout_set(&g_fwbank_dump.tm, FWBANK_DUMP_RETRY_DELAY * 1000);

		BBF_ERR("Attempt %d/%d: fwbank dump blob buf is empty. Retrying in %d second(s)...",
					g_retry_count, FWBANK_DUMP_MAX_RETRIES, FWBANK_DUMP_RETRY_DELAY);
	} else {
		BBF_ERR("Max retries (%d) reached: The fwbank dump buffer is empty. Unable to register 'fwbank' ubus object",
				FWBANK_DUMP_MAX_RETRIES);
	}
}

static int init_global_fwbank_dump(void)
{
	BBF_DEBUG("Initializing global fwbank dump");

	int res = sysmngr_task_fork(fwbank_dump_finish_callback, FWBANK_DUMP_CMD, 10, NULL, 0);
	if (res) {
		BBF_ERR("Failed to start task for fwbank dump command");
		return -1;
	}

	BBF_DEBUG("fwbank dump blob initialized successfully");
	return 0;
}

static void fwbank_listen_timeout(struct uloop_timeout *timeout)
{
	struct fwbank_event_data *data = NULL;

	BBF_DEBUG("fwbank listen timeout triggered");

	data = container_of(timeout, struct fwbank_event_data, tm);
	if (data == NULL)
		return;

	ubus_unregister_event_handler(data->ctx, &data->ev);
}

static void fwbank_receive_sysupgrade(struct ubus_context *ctx, struct ubus_event_handler *ev,
				const char *type, struct blob_attr *msg)
{
	struct fwbank_event_data *data = NULL;
	struct blob_attr *cur = NULL;
	char bank_id_str[16] = {0};
	int rem;

	if (!msg || !ev) {
		BBF_ERR("Invalid event data in sysupgrade handler");
		return;
	}

	data = container_of(ev, struct fwbank_event_data, ev);
	if (data == NULL)
		return;

	if (data->band_id < 0) { // bank_id should be a valid id
		BBF_ERR("Invalid bank_id: %d", data->band_id);
		return;
	}

	snprintf(bank_id_str, sizeof(bank_id_str), "%u", data->band_id);

	blobmsg_for_each_attr(cur, msg, rem) {
		if (DM_STRCMP("bank_id", blobmsg_name(cur)) == 0) {
			char *attr_val = (char *)blobmsg_data(cur);
			if (DM_STRCMP(attr_val, bank_id_str) != 0) {
				BBF_ERR("Mismatched bank_id (%s != %s)", attr_val, bank_id_str);
				return;
			}
		}

		if (DM_STRCMP("status", blobmsg_name(cur)) == 0) {
			char *attr_val = (char *)blobmsg_data(cur);

			if (DM_STRCMP(attr_val, "Downloading") == 0) {
				BBF_DEBUG("Sysupgrade status: Downloading");
				return;
			}

			if (DM_STRCMP(attr_val, "Available") == 0) {
				BBF_DEBUG("Sysupgrade status: Available. Refreshing fwbank dump.");
				init_global_fwbank_dump();
				break;
			}
		}
	}

	if (data->tm.pending) {
		// Cancel timeout if the sysupgrade status available is found
		uloop_timeout_cancel(&data->tm);
	}

	ubus_unregister_event_handler(data->ctx, &data->ev);
	return;
}

static struct fwbank_event_data g_fwbank_event_data = {
	.tm.cb = fwbank_listen_timeout,
	.ev.cb = fwbank_receive_sysupgrade,
	.timeout = 120
};

static void fwbank_wait_for_sysupgrade_event(struct ubus_context *ctx, uint32_t band_id)
{
	BBF_DEBUG("Waiting for sysupgrade event for bank_id: %u", band_id);

	g_fwbank_event_data.ctx = ctx;
	g_fwbank_event_data.band_id = band_id;

	ubus_register_event_handler(ctx, &g_fwbank_event_data.ev, "sysupgrade");
	uloop_timeout_set(&g_fwbank_event_data.tm, g_fwbank_event_data.timeout * 1000);
}

static bool is_set_bootbank_success(struct blob_buf *output_bb)
{
	struct blob_attr *tb = NULL;
	struct blobmsg_policy policy = {
		.name = "success",
		.type = BLOBMSG_TYPE_BOOL,
	};

	// Parse the blob buffer for the "success" field
	if (blobmsg_parse(&policy, 1, &tb, blobmsg_data(output_bb->head), blobmsg_len(output_bb->head)) != 0) {
		BBF_ERR("Failed to parse blobmsg data");
		return false;
	}

	// Check if the "success" field exists and is of the correct type
	if (tb && blobmsg_type(tb) == BLOBMSG_TYPE_BOOL)
		return blobmsg_get_bool(tb);

	return false;
}

static bool is_upgrade_success(struct blob_buf *output_bb)
{
	struct blob_attr *tb = NULL;
	struct blobmsg_policy policy = {
		.name = "result",
		.type = BLOBMSG_TYPE_STRING,
	};

	// Parse the blob buffer for the "result" field
	if (blobmsg_parse(&policy, 1, &tb, blobmsg_data(output_bb->head), blobmsg_len(output_bb->head)) != 0) {
		BBF_ERR("Failed to parse blobmsg data");
		return false;
	}

	// Check if the "result" field exists and is of the correct type
	if (tb && blobmsg_type(tb) == BLOBMSG_TYPE_STRING)
		return (strcmp(blobmsg_get_string(tb), "ok") == 0) ? true : false;

	return false;
}

struct blob_buf *sysmngr_fwbank_dump(void)
{
	return &g_fwbank_dump.output;
}

static void fwbank_set_bootbank_finish_callback(struct ubus_context *ctx, struct ubus_request_data *req, int *pipe_fds, uint32_t bank_id)
{
	BBF_DEBUG("Task finished Line=%d && func=%s", __LINE__, __func__);

	close(pipe_fds[1]); // Close unused write end

	char buffer[1024] = {0};
	ssize_t bytes_read;

	BBF_DEBUG("Reading script output...");

	// Read the output from the script
	while ((bytes_read = read(pipe_fds[0], buffer, sizeof(buffer) - 1)) > 0) {
		buffer[bytes_read] = '\0'; // Null-terminate the buffer
		BBF_DEBUG("Script output: %s", buffer);
	}

	close(pipe_fds[0]); // Close read end

	if (bytes_read < 0) {
		BBF_ERR("Failed to read from pipe");
		return;
	}

	struct blob_buf setbootbank_bb = {0};

	memset(&setbootbank_bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&setbootbank_bb, 0);

	if (!blobmsg_add_json_from_string(&setbootbank_bb, buffer)) {
		BBF_ERR("Failed to create blob buf");
		blob_buf_free(&setbootbank_bb);
		return;
	}

	bool is_success = is_set_bootbank_success(&setbootbank_bb);

	if (ctx && req) {
		BBF_DEBUG("Send ubus output");
		ubus_send_reply(ctx, req, setbootbank_bb.head);
	}

	blob_buf_free(&setbootbank_bb);

	if (is_success) {
		// Update fwbank dump output
		init_global_fwbank_dump();
	}

	return;
}

int sysmngr_fwbank_set_bootbank(uint32_t bank_id, struct ubus_request_data *req)
{
	char cmd[128] = {0};

	snprintf(cmd, sizeof(cmd), "echo '{\"bank\":%u}' | %s 2>/dev/null", bank_id, FWBANK_SET_BOOTBANK_CMD);

	int res = sysmngr_task_fork(fwbank_set_bootbank_finish_callback, cmd, 5, req, 0);
	if (res) {
		BBF_ERR("Failed to start task for fwbank set bootbank command");
		return -1;
	}

	return 0;
}

static void fwbank_upgrade_finish_callback(struct ubus_context *ctx, struct ubus_request_data *req, int *pipe_fds, uint32_t bank_id)
{
	BBF_DEBUG("Task finished Line=%d && func=%s", __LINE__, __func__);

	close(pipe_fds[1]); // Close unused write end

	char buffer[1024] = {0};
	ssize_t bytes_read;

	BBF_DEBUG("Reading script output...");

	// Read the output from the script
	while ((bytes_read = read(pipe_fds[0], buffer, sizeof(buffer) - 1)) > 0) {
		buffer[bytes_read] = '\0'; // Null-terminate the buffer
		BBF_DEBUG("Script output: %s", buffer);
	}

	close(pipe_fds[0]); // Close read end

	if (bytes_read < 0) {
		BBF_ERR("Failed to read from pipe");
		return;
	}

	struct blob_buf upgrade_bb = {0};

	memset(&upgrade_bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&upgrade_bb, 0);

	if (!blobmsg_add_json_from_string(&upgrade_bb, buffer)) {
		BBF_ERR("Failed to create blob buf");
		blob_buf_free(&upgrade_bb);
		return;
	}

	bool is_success = is_upgrade_success(&upgrade_bb);

	if (ctx && req) {
		BBF_DEBUG("Send ubus output");
		ubus_send_reply(ctx, req, upgrade_bb.head);
	}

	blob_buf_free(&upgrade_bb);

	if (ctx && is_success) {
		// Update fwbank dump output after getting sysupgrade event with status available
		// { "sysupgrade": {"status":"Available","bank_id":"x"} }
		fwbank_wait_for_sysupgrade_event(ctx, bank_id);
	}

	return;
}

int sysmngr_fwbank_upgrade(const char *path, bool auto_activate, uint32_t bank_id, bool keep_settings, struct ubus_request_data *req)
{
	char cmd[1024] = {0};

	snprintf(cmd, sizeof(cmd), "echo '{\"path\":\"%s\", \"auto_activate\":%d, \"bank\":%u, \"keep_settings\":%d}' | %s 2>/dev/null",
			path, auto_activate, bank_id, keep_settings, FWBANK_UPGRADE_CMD);

	int res = sysmngr_task_fork(fwbank_upgrade_finish_callback, cmd, 10, req, bank_id);
	if (res) {
		BBF_ERR("Failed to start task for fwbank upgrade command");
		return -1;
	}

	return 0;
}

static int dump_handler(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	ubus_send_reply(ctx, req, g_fwbank_dump.output.head);
	return 0;
}

enum {
	SET_BOOT_BANK,
	__SET_BOOT_MAX
};

static const struct blobmsg_policy set_bootbank_policy[] = {
	[SET_BOOT_BANK] = { .name = "bank", .type = BLOBMSG_TYPE_INT32 },
};

static int set_bootbank_handler(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__SET_BOOT_MAX];
	uint32_t band_id = 0;
	int res = 0;

	if (blobmsg_parse(set_bootbank_policy, __SET_BOOT_MAX, tb, blob_data(msg), blob_len(msg))) {
		BBF_ERR("Failed to parse the 'set_bootbank' message");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	if (tb[SET_BOOT_BANK])
		band_id = blobmsg_get_u32(tb[SET_BOOT_BANK]);

	res = sysmngr_fwbank_set_bootbank(band_id, req);

	if (res) {
		struct blob_buf bb = {0};

		memset(&bb, 0, sizeof(struct blob_buf));
		blob_buf_init(&bb, 0);
		blobmsg_add_u8(&bb, "status", false);
		ubus_send_reply(ctx, req, bb.head);
		blob_buf_free(&bb);
	}

	return 0;
}

enum {
	UPGRADE_PATH,
	UPGRADE_AUTO_ACTIVATE,
	UPGRADE_BANK,
	UPGRADE_KEEP_SETTINGS,
	__UPGRADE_MAX
};

static const struct blobmsg_policy upgrade_policy[] = {
	[UPGRADE_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
	[UPGRADE_AUTO_ACTIVATE] = { .name = "auto_activate", .type = BLOBMSG_TYPE_BOOL },
	[UPGRADE_BANK] = { .name = "bank", .type = BLOBMSG_TYPE_INT32 },
	[UPGRADE_KEEP_SETTINGS] = { .name = "keep_settings", .type = BLOBMSG_TYPE_BOOL},
};

static int upgrade_handler(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__UPGRADE_MAX];
	char *fw_path = NULL;
	uint32_t bank_id = 0;
	bool auto_activate = false;
	bool keep_settings = false;
	int res = 0;

	if (blobmsg_parse(upgrade_policy, __UPGRADE_MAX, tb, blob_data(msg), blob_len(msg))) {
		BBF_ERR("Failed to parse the 'upgrade' message");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	if (tb[UPGRADE_PATH])
		fw_path = blobmsg_get_string(tb[UPGRADE_PATH]);

	if (tb[UPGRADE_AUTO_ACTIVATE])
		auto_activate = blobmsg_get_bool(tb[UPGRADE_AUTO_ACTIVATE]);

	if (tb[UPGRADE_BANK])
		bank_id = blobmsg_get_u32(tb[UPGRADE_BANK]);

	if (tb[UPGRADE_KEEP_SETTINGS])
		keep_settings = blobmsg_get_bool(tb[UPGRADE_KEEP_SETTINGS]);

	res = sysmngr_fwbank_upgrade(fw_path, auto_activate, bank_id, keep_settings, req);

	if (res) {
		struct blob_buf bb = {0};

		memset(&bb, 0, sizeof(struct blob_buf));
		blob_buf_init(&bb, 0);
		blobmsg_add_string(&bb, "result", "failure");
		ubus_send_reply(ctx, req, bb.head);
		blob_buf_free(&bb);
	}

	return 0;
}

static struct ubus_method fwbank_methods[] = {
	UBUS_METHOD_NOARG("dump", dump_handler),
	UBUS_METHOD("set_bootbank", set_bootbank_handler, set_bootbank_policy),
	UBUS_METHOD("upgrade", upgrade_handler, upgrade_policy),
};

static struct ubus_object_type fwbank_type = UBUS_OBJECT_TYPE("fwbank", fwbank_methods);

static struct ubus_object fwbank_object = {
	.name = "fwbank",
	.type = &fwbank_type,
	.methods = fwbank_methods,
	.n_methods = ARRAY_SIZE(fwbank_methods)
};

int sysmngr_register_fwbank(struct ubus_context *ubus_ctx)
{
	int res = ubus_add_object(ubus_ctx, &fwbank_object);
	if (res) {
		BBF_ERR("Failed to register 'fwbank' ubus object!!!!!!");
		return -1;
	}

	BBF_INFO("'fwbank' ubus object was registered");
	return res;
}

int sysmngr_unregister_fwbank(struct ubus_context *ubus_ctx)
{
	ubus_remove_object(ubus_ctx, &fwbank_object);

	BBF_INFO("'fwbank' ubus object was unregistered, and resources were freed");
	return 0;
}

int sysmngr_init_fwbank_dump(struct ubus_context *ubus_ctx)
{
	int res = 0;

	g_fwbank_dump.ctx = ubus_ctx;
	g_fwbank_dump.tm.cb = fwbank_dump_timer;

	if (!file_exists(FWBANK_FILE_PATH)) {
		BBF_ERR("The fwbank file (%s) is missing", FWBANK_FILE_PATH);
		return -1;
	}

	res = init_global_fwbank_dump();
	if (res) {
		BBF_ERR("Failed to fetch 'fwbank' output or no data available");
		return -1;
	}

	return 0;
}

int sysmngr_clean_fwbank_dump(struct ubus_context *ubus_ctx)
{
	free_global_fwbank_dump(&g_fwbank_dump.output);
	return 0;
}
