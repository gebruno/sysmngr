# Proposal for copy config on firmware change

Currently we have different behaviour of config management based on following scenarios, which could be bit confusing for operators and end-users.

## Scenario 1: Upgrade using ACS using '1 Firmware Image' and '6 Stored image'

In this scenario, we provide a firmware default uci option to configure the config management

```
cwmp.cpe.fw_upgrade_keep_settings
```

If this option is enabled, then config is copied over to the next firmware.

> Note: there is no way to change this behaviour from ACS, we only provide uci option, but no datamodel vendor extension

## Scenario 2: Upgrade using USP Controller

With USP it has many possibilities, user can

1. Download and activate at the same time
2. Download without Activate
3. Activate the downloaded image immediately 
4. Activate the downloaded image with a time-window

Currently we do provide a vendor extended option in `Download()` command to manage the config
```
Device.DeviceInfo.FirmwareImage.{i}.Download() input:AutoActivate
Device.DeviceInfo.FirmwareImage.{i}.Download() input:X_IOWRT_EU_KeepConfig
```

But, same is lacking in Activate() operate command
```
Device.DeviceInfo.FirmwareImage.{i}.Activate()
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.End
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.Mode
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.Start
```

## Scenario 3: Switch the firmware with DM without upgrade

Currently there is no way to manage the config, when user switches the bank using `Set` operation on `BootFirmwareImage`

```
Device.DeviceInfo.ActiveFirmwareImage
Device.DeviceInfo.BootFirmwareImage
```

## Proposal: Unified config management

In place of having separate but similar change in cwmp and usp, proposal is to have it in `sysmngr` config, and that will be used by both `ACS` and `USP Controller`.

```bash
# cat /etc/config/sysmngr

config globals 'globals'
        option log_level '4'
	option keep_config 'Full'
```

keep_config should be an Enum, so that it can be updated later on for future extensions

| Enum value  | Meaning   |
| ----------  | --------- |
| Full        | Similar to sysupgrade -b, copies full configuration |
| None        | Do not copy config to the next firmware |

This table can be further extended in the future with (how they will work is out of scope of this document),

Datamodel => Meaning copy only datamodel config

For datamodel Integration, we can add a 'rw' vendor extension to map with this new uci option

Device.DeviceInfo.X_IOWRT_EU_ConfigBackup => sysmngr.globals.keep_config

With this, operator can manage the Firmware upgrade behaviour from all different scenarios.
