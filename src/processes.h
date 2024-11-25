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

#ifndef __PROCESSES_H
#define __PROCESSES_H

extern DMOBJ tDeviceInfoProcessStatusObj[];
extern DMLEAF tDeviceInfoProcessStatusParams[];

void sysmngr_process_init(struct ubus_context *ubus_ctx);
void sysmngr_process_clean(struct ubus_context *ubus_ctx);
void sysmngr_cpu_init(void);
void sysmngr_cpu_clean(void);

#endif //__PROCESSES_H
