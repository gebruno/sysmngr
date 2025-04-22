# Design For Dual FirmwareImage Download and Activation

According to the TR181 data model, dual boot bank is represented by `Device.DeviceInfo.FirmwareImage.` multi-instance object

```
Device.DeviceInfo.FirmwareImage.{i}.Alias
Device.DeviceInfo.FirmwareImage.{i}.Available
Device.DeviceInfo.FirmwareImage.{i}.BootFailureLog
Device.DeviceInfo.FirmwareImage.{i}.Name
Device.DeviceInfo.FirmwareImage.{i}.Status
Device.DeviceInfo.FirmwareImage.{i}.Version
```

This object also provides two operate commands to manage the firmware image using USP(TR-369)

1. Download() operate command is basically used for download a new firmware image and apply it
```
Device.DeviceInfo.FirmwareImage.{i}.Download()
Device.DeviceInfo.FirmwareImage.{i}.Download() input:AutoActivate
Device.DeviceInfo.FirmwareImage.{i}.Download() input:CheckSum
Device.DeviceInfo.FirmwareImage.{i}.Download() input:CheckSumAlgorithm
Device.DeviceInfo.FirmwareImage.{i}.Download() input:CommandKey
Device.DeviceInfo.FirmwareImage.{i}.Download() input:FileSize
Device.DeviceInfo.FirmwareImage.{i}.Download() input:Password
Device.DeviceInfo.FirmwareImage.{i}.Download() input:URL
Device.DeviceInfo.FirmwareImage.{i}.Download() input:Username
Device.DeviceInfo.FirmwareImage.{i}.Download() input:X_IOWRT_EU_KeepConfig
```

> Note: `X_IOWRT_EU_KeepConfig` is a vendor extension and provide additional functionality, its not a mandatory parameter and only visible if vendor extension enabled.


2. Activate() operate command to activate the downloaded firmware or switch to other available bank

```
Device.DeviceInfo.FirmwareImage.{i}.Activate()
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.End
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.MaxRetries
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.Mode
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.Start
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:TimeWindow.{i}.UserMessage
Device.DeviceInfo.FirmwareImage.{i}.Activate() input:X_IOWRT_EU_KeepConfig
```

> Note: `X_IOWRT_EU_KeepConfig` is a vendor extension and provide additional functionality, its not a mandatory parameter and only visible if vendor extension enabled.

Apart from these it also has `Device.DeviceInfo.MaxNumberOfActivateTimeWindows` which defines the maximum available timewindows for `Activate()` operate command.

Here, TR-181 definition for parameters and operate commands works well for most of the tree, except the Vendor extension functionality and TimeWindow management, which is cpe dependent.

## Vendor extensions

With Firmware management, one unaddressed topic is which is very critical for actual deployment is, "What to do with (user/operate) config changes done on the current firmware?"

To give operate/user more flexibility to make that decision, we provide datamodel Vendor extensions to decide at the time of firmware Download/Activate, with below design


1. In Download() operate command,
  a. if AutoActivate=true, then only use the provided X_IOWRT_EU_KeepConfig value, in case of KeepConfig not provided use default keep_config=1
  b. if AutoActivate=false or not provided then ignore X_IOWRT_EU_KeepConfig value, in this case download will be done without keeping config
2. In Activate() operate command
  a. Copy the current config to the new bank, if X_IOWRT_EU_KeepConfig is provided and true
  b. If X_IOWRT_EU_KeepConfig not provided then use "1" as default keep config value
3. Bank switch using DeviceInfo.BootFirmwareImage
  a. In case of boot back switch using BootFirmwareImage, config will not be copied

## Activate command handling

The Activate() command is an operation to activate the firmware image immediately or schedule it in another time using TimeWindow.

In datamodel the TimeWindow is handled by using crontab.

Below is an example of an 'Activate()' command call with three TimeWindow instances. As a result, three jobs are created according to the defined TimeWindow.{i}.Start:

```bash
root@iopsys-44d43771aff0:~# ubus call bbfdm operate '{"path":"Device.DeviceInfo.FirmwareImage.2.", "action":"Activate()", "input":{"TimeWindow.1
.Start":"1800", "TimeWindow.1.End":"3600", "TimeWindow.1.Mode":"WhenIdle", "TimeWindow.2.Start":"5400", "TimeWindow.2.End":"9000", "TimeWindow
.2.Mode":"WhenIdle", "TimeWindow.3.Start":"86400", "TimeWindow.3.End":"172800", "TimeWindow.3.Mode":"Immediately"}}'
{
	"Results": [
		{
			"path": "Device.DeviceInfo.FirmwareImage.2.Activate()",
			"result": [
				{
					
				}
			]
		}
	]
}
root@iopsys-44d43771aff0:~# crontab -l
52 22 21 2 * sh /usr/share/bbfdm/scripts/bbf_activate_handler.sh 'WhenIdle' '2' '1800' '0' '' ''
52 23 21 2 * sh /usr/share/bbfdm/scripts/bbf_activate_handler.sh 'WhenIdle' '2' '3600' '0' '' ''
22 22 22 2 * sh /usr/share/bbfdm/scripts/bbf_activate_handler.sh 'Immediately' '2' '86400' '1' '' ''
root@iopsys-44d43771aff0:~# 

```

For those cron jobs it is required to give the handler script to be executed which is in our case [bbf_activate_handler.sh](../../libbbfdm/scripts/bbf_activate_handler.sh). And, it is located under '/usr/share/bbfdm/scripts/' in the device.


### Activate Handler for different Mode support

The aim of `bbf_activate_handler.sh` script is to manage firmware images based on the **mode** and the other passed arguments.


#### 1. Mode 'AnyTime' and 'Immediately':

For these modes and based on the firmware bank id, the required firmware image will be immediately activated at start time. The TimeWindow.{i}.End is ignored.

#### 2. How to handle 'WhenIdle' mode:

Definition of WhenIdle may vary for each deployment and customer, to make it customizable [bbf_check_idle.sh](../../libbbfdm/scripts/bbf_check_idle.sh) script is used. It is assumed that customer shall overwrite this file using customer-config to match with there requirement.

In this mode, [bbf_activate_handler.sh](../../libbbfdm/scripts/bbf_activate_handler.sh) script calls this script [bbf_check_idle.sh](../../libbbfdm/scripts/bbf_check_idle.sh) to determine the idle state of the device. [bbf_activate_handler.sh](../../libbbfdm/scripts/bbf_activate_handler.sh) assumes the device as idle if the exit status of the above script is 0, or if the [bbf_check_idle.sh](../../libbbfdm/scripts/bbf_check_idle.sh) is not present in the predefined path "ACTIVATE_HANDLER_FILE@dmcommon.h".


If the exit code from the idle script is zero then firmware image can be activated. Otherwise, it has to wait for next time slot which is defined by 'RETRY_TIME' variable.

> Note1: The time slot is set through 'RETRY_TIME' variable which is defined under '/usr/share/bbfdm/scripts/bbf_activate_handler.sh' script.

> Note2: The exit status of the script [bbf_check_idle.sh](../../libbbfdm/scripts/bbf_check_idle.sh) is important because based on it, the '[bbf_activate_handler.sh](../../libbbfdm/scripts/bbf_activate_handler.sh) script will decide whether the image can be activated or not.

> Note3: Algorithm/Logic to determine the Idle state of device is out of scope of this document and it is expected that users overwrite this script with the logic to determine the same in actual deployment.

> Note4: If 1 or more TimeWindow.{i}.Mode is set to 'WhenIdle' and all of them fails to get the idle state. The latest TimeWindow instance will force the device to activate the firmware image.

> Note5: If the idle script [bbf_check_idle.sh](../../libbbfdm/scripts/bbf_check_idle.sh) not present in the pre-defined path "ACTIVATE_HANDLER_FILE@dmcommon.h", then the device is assumed to be in ideal state and the firmware shall be activated instantly.

> Note6: It is very likely that TimeWindow with 'WhenIdle' mode might not find any suitable Idle state, in that case firmware shall not be activated. If users/operators want to make sure that firmware gets activated at the end, then they can add a TimeWindow with 'AnyTime/Immediate' mode at the end, to activate the firmware.


#### Good to know

* TimeWindow instance arguments are optional.

* TimeWindow instances attributes must not overlap.

* If TimeWindow.{i}.Start is set, TimeWindow.{i}.End and TimeWindow.{i}.Mode become mondatory.

* The firmware activation is done by [bbf_activate_handler.sh](../../libbbfdm/scripts/bbf_activate_handler.sh) script as per the defined Mode in TimeWindow, but if the TimeWindow is not defined, it will activate the requested FirmwareImage instance immediately.

* If the customer wants to be sure that the required firmware is getting activated at the end then they can define the TimeWindow.{i}.Mode as 'AnyTime' or 'Immediately' in the last TimeWindow instance.

* This document is only target for Firmware management using USP.

* TimeWindow.{i}.Mode = 'ConfirmationNeeded' is not supported.

