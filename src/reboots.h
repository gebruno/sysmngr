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

#ifndef __REBOOTS_H
#define __REBOOTS_H

extern DMOBJ tDeviceInfoRebootsObj[];
extern DMLEAF tDeviceInfoRebootsParams[];

void sysmngr_reboots_init(void);

#endif //__REBOOTS_H
