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

#ifndef __FWBANK_H
#define __FWBANK_H

extern struct blobmsg_policy sysmngr_dump_policy[];
extern struct blobmsg_policy sysmngr_bank_policy[];

struct blob_buf *sysmngr_fwbank_dump(void);
int sysmngr_fwbank_set_bootbank(uint32_t bank_id, struct ubus_request_data *req);
int sysmngr_fwbank_upgrade(const char *path, bool auto_activate, uint32_t bank_id, bool keep_settings, struct ubus_request_data *req);

void sysmngr_fwbank_refresh_global_dump(void);

int sysmngr_register_fwbank(struct ubus_context *ubus_ctx);
int sysmngr_unregister_fwbank(struct ubus_context *ubus_ctx);

int sysmngr_init_fwbank_dump(struct ubus_context *ubus_ctx);
int sysmngr_clean_fwbank_dump(struct ubus_context *ubus_ctx);

#endif //__FWBANK_H
