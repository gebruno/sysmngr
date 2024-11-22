/*
 * Copyright (C) 2022-2024 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 */

#ifndef __FW_IMAGES_H
#define __FW_IMAGES_H

extern DMLEAF tDeviceInfoFirmwareImageParams[];
extern char fw_image_dependency[];

int browseDeviceInfoFirmwareImageInst(struct dmctx *dmctx, DMNODE *parent_node, void *prev_data, char *prev_instance);

int get_device_active_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value);
int get_device_boot_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value);
int set_device_boot_fwimage(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action);
int get_DeviceInfo_MaxNumberOfActivateTimeWindows(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value);
int get_DeviceInfo_FirmwareImageNumberOfEntries(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value);

#endif //__FW_IMAGES_H
