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

#include <libbbfdm-api/dmcommon.h>

#ifdef SYSMNGR_VENDOR_CONFIG_FILE
#include "configs.h"
#endif

#ifdef SYSMNGR_FIRMWARE_IMAGE
#include "fw_images.h"
#endif

#ifdef SYSMNGR_MEMORY_STATUS
#include "memory.h"
#endif

#ifdef SYSMNGR_PROCESS_STATUS
#include "processes.h"
#endif

#ifdef SYSMNGR_REBOOTS
#include "reboots.h"
#endif

#ifdef SYSMNGR_SUPPORTED_DATA_MODEL
#include "supported_dm.h"
#endif

#ifdef SYSMNGR_NETWORK_PROPERTIES
#include "network.h"
#endif

/*************************************************************
* GET & SET PARAM
**************************************************************/
static int get_device_devicecategory(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "DeviceCategory", value);
	return 0;
}

static int get_device_manufacturer(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp","cpe","manufacturer", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "Manufacturer", value);

	return 0;
}

static int get_device_manufactureroui(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "manufacturer_oui", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "ManufacturerOUI", value);

	return 0;
}

static int get_device_modelname(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "model_name", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "ModelName", value);
	return 0;
}

static int get_device_description(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "description", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "Description", value);
	return 0;
}

static int get_device_productclass(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "product_class", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "ProductClass", value);

	return 0;
}

static int get_device_serialnumber(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "serial_number", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "SerialNumber", value);

	return 0;
}

static int get_device_hardwareversion(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "HardwareVersion", value);
	return 0;
}

static int get_device_softwareversion(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("cwmp", "cpe", "software_version", value);
	if (*value[0] == '\0')
		db_get_value_string("device", "deviceinfo", "SoftwareVersion", value);

	return 0;
}

static int get_device_additionalhardwareversion(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "AdditionalHardwareVersion", value);
	return 0;
}

static int get_device_additionalsoftwareversion(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "AdditionalSoftwareVersion", value);
	return 0;
}

static int get_device_provisioningcode(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	char *dhcp = NULL, *provisioning_code = NULL, *dhcp_provisioning_code = NULL;
	bool discovery = false;

	dmuci_get_option_value_string("cwmp", "acs", "dhcp_discovery", &dhcp);
	dmuci_get_option_value_string("cwmp", "cpe", "provisioning_code", &provisioning_code);
	dmuci_get_option_value_string("cwmp", "cpe", "dhcp_provisioning_code", &dhcp_provisioning_code);

	discovery = dmuci_string_to_boolean(dhcp);

	if ((discovery == true) && (DM_STRLEN(dhcp_provisioning_code) != 0))
		*value = dhcp_provisioning_code;
	else
		*value = provisioning_code;

	return 0;
}

static int set_device_provisioningcode(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action) {
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, value, -1, 64, NULL, NULL))
				return FAULT_9007;
			return 0;
		case VALUESET:
			dmuci_set_value("cwmp", "cpe", "provisioning_code", value);
			return 0;
	}
	return 0;
}

static int get_device_info_uptime(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	*value = get_uptime();
	return 0;
}

static int get_device_info_firstusedate(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("time", "global", "first_use_date", value);
	return 0;
}

static int get_deviceinfo_cid (char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "CID", value);
	return 0;
}

static int get_deviceinfo_friendlyname(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "FriendlyName", value);
	return 0;
}

static int get_deviceinfo_pen (char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "PEN", value);
	return 0;
}

static int get_deviceinfo_modelnumber (char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "ModelNumber", value);
	return 0;
}

static int get_DeviceInfo_HostName(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	dmuci_get_option_value_string("system", "@system[0]", "hostname", value);
	return 0;
}

static int set_DeviceInfo_HostName(char *refparam, struct dmctx *ctx, void *data, char *instance, char *value, int action)
{
	switch (action)	{
		case VALUECHECK:
			if (bbfdm_validate_string(ctx, value, -1, 255, NULL, NULL))
				return FAULT_9007;
			break;
		case VALUESET:
			dmuci_set_value("system", "@system[0]", "hostname", value);
			break;
	}
	return 0;
}

#ifdef SYSMNGR_VENDOR_EXTENSIONS
static int get_deviceinfo_base_mac_addr(char *refparam, struct dmctx *ctx, void *data, char *instance, char **value)
{
	db_get_value_string("device", "deviceinfo", "BaseMACAddress", value);
	return 0;
}
#endif

/**********************************************************************************************************************************
*                                            OBJ & LEAF DEFINITION
***********************************************************************************************************************************/
/* *** Device.DeviceInfo. *** */
DMOBJ tDeviceInfoObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type*/
#ifdef SYSMNGR_VENDOR_CONFIG_FILE
{"VendorConfigFile", &DMREAD, NULL, NULL, NULL, browseVcfInst, NULL, NULL, NULL, tDeviceInfoVendorConfigFileParams, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_MEMORY_STATUS
{"MemoryStatus", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoMemoryStatusObj, tDeviceInfoMemoryStatusParams, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_PROCESS_STATUS
{"ProcessStatus", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoProcessStatusObj, tDeviceInfoProcessStatusParams, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_NETWORK_PROPERTIES
{"NetworkProperties", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoNetworkPropertiesParams, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_SUPPORTED_DATA_MODEL
{"SupportedDataModel", &DMREAD, NULL, NULL, NULL, browseDeviceInfoSupportedDataModelInst, NULL, NULL, NULL, tDeviceInfoSupportedDataModelParams, NULL, BBFDM_CWMP},
#endif

#ifdef SYSMNGR_FIRMWARE_IMAGE
{"FirmwareImage", &DMREAD, NULL, NULL, fw_image_dependency, browseDeviceInfoFirmwareImageInst, NULL, NULL, NULL, tDeviceInfoFirmwareImageParams, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_REBOOTS
{"Reboots", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoRebootsObj, tDeviceInfoRebootsParams, NULL, BBFDM_USP},
#endif

{0}
};

DMLEAF tDeviceInfoParams[] = {
/* PARAM, permission, type, getvalue, setvalue, bbfdm_type*/
{"DeviceCategory", &DMREAD, DMT_STRING, get_device_devicecategory, NULL, BBFDM_BOTH},
{"Manufacturer", &DMREAD, DMT_STRING, get_device_manufacturer, NULL, BBFDM_BOTH},
{"ManufacturerOUI", &DMREAD, DMT_STRING, get_device_manufactureroui, NULL, BBFDM_BOTH},
{"ModelName", &DMREAD, DMT_STRING, get_device_modelname, NULL, BBFDM_BOTH},
{"Description", &DMREAD, DMT_STRING, get_device_description, NULL, BBFDM_BOTH},
{"ProductClass", &DMREAD, DMT_STRING, get_device_productclass, NULL, BBFDM_BOTH},
{"SerialNumber", &DMREAD, DMT_STRING, get_device_serialnumber, NULL, BBFDM_BOTH},
{"HardwareVersion", &DMREAD, DMT_STRING, get_device_hardwareversion, NULL, BBFDM_BOTH},
{"SoftwareVersion", &DMREAD, DMT_STRING, get_device_softwareversion, NULL, BBFDM_BOTH},
{"AdditionalHardwareVersion", &DMREAD, DMT_STRING, get_device_additionalhardwareversion, NULL, BBFDM_BOTH},
{"AdditionalSoftwareVersion", &DMREAD, DMT_STRING, get_device_additionalsoftwareversion, NULL, BBFDM_BOTH},
{"ProvisioningCode", &DMWRITE, DMT_STRING, get_device_provisioningcode, set_device_provisioningcode, BBFDM_BOTH},
{"UpTime", &DMREAD, DMT_UNINT, get_device_info_uptime, NULL, BBFDM_BOTH},
{"FirstUseDate", &DMREAD, DMT_TIME, get_device_info_firstusedate, NULL, BBFDM_BOTH},
{"CID", &DMREAD, DMT_STRING, get_deviceinfo_cid, NULL, BBFDM_USP},
{"FriendlyName", &DMREAD, DMT_STRING, get_deviceinfo_friendlyname, NULL, BBFDM_USP},
{"PEN", &DMREAD, DMT_STRING, get_deviceinfo_pen, NULL, BBFDM_USP},
{"ModelNumber", &DMREAD, DMT_STRING, get_deviceinfo_modelnumber, NULL, BBFDM_BOTH},
{"HostName", &DMWRITE, DMT_STRING, get_DeviceInfo_HostName, set_DeviceInfo_HostName, BBFDM_BOTH},

#ifdef SYSMNGR_VENDOR_CONFIG_FILE
{"VendorConfigFileNumberOfEntries", &DMREAD, DMT_UNINT, get_DeviceInfo_VendorConfigFileNumberOfEntries, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_SUPPORTED_DATA_MODEL
{"SupportedDataModelNumberOfEntries", &DMREAD, DMT_UNINT, get_DeviceInfo_SupportedDataModelNumberOfEntries, NULL, BBFDM_CWMP},
#endif

#ifdef SYSMNGR_FIRMWARE_IMAGE
{"ActiveFirmwareImage", &DMREAD, DMT_STRING, get_device_active_fwimage, NULL, BBFDM_BOTH, DM_FLAG_REFERENCE},
{"BootFirmwareImage", &DMWRITE, DMT_STRING, get_device_boot_fwimage, set_device_boot_fwimage, BBFDM_BOTH, DM_FLAG_REFERENCE},
{"MaxNumberOfActivateTimeWindows", &DMREAD, DMT_UNINT, get_DeviceInfo_MaxNumberOfActivateTimeWindows, NULL, BBFDM_USP},
{"FirmwareImageNumberOfEntries", &DMREAD, DMT_UNINT, get_DeviceInfo_FirmwareImageNumberOfEntries, NULL, BBFDM_BOTH},
#endif

#ifdef SYSMNGR_VENDOR_EXTENSIONS
{BBF_VENDOR_PREFIX"BaseMACAddress", &DMREAD, DMT_STRING, get_deviceinfo_base_mac_addr, NULL, BBFDM_BOTH},
#endif

{0}
};

/* *** Device. *** */
DMOBJ tDMDeviceInfoObj[] = {
/* OBJ, permission, addobj, delobj, checkdep, browseinstobj, nextdynamicobj, dynamicleaf, nextobj, leaf, linker, bbfdm_type*/
{"DeviceInfo", &DMREAD, NULL, NULL, NULL, NULL, NULL, NULL, tDeviceInfoObj, tDeviceInfoParams, NULL, BBFDM_BOTH},
{0}
};

DM_MAP_OBJ tDynamicObj[] = {
/* parentobj, nextobject, parameter */
{"Device.", tDMDeviceInfoObj, NULL},
{0}
};
