PROG = sysmngr

OBJS = sysmngr.o deviceinfo.o utils.o

PROG_CFLAGS = $(CFLAGS) -Wall -Werror -fstrict-aliasing -g

PROG_LDFLAGS = $(LDFLAGS) -luci -lubus -lubox -lblobmsg_json -ljson-c -lssl -lcrypto -lbbfdm-ubus -lbbfdm-api -lbbfdm-api-v2
FPIC := -fPIC

ifeq ($(SYSMNGR_VENDOR_CONFIG_FILE),y)
OBJS += configs.o
PROG_CFLAGS += -DSYSMNGR_VENDOR_CONFIG_FILE
endif

ifeq ($(SYSMNGR_MEMORY_STATUS),y)
OBJS += memory.o
PROG_CFLAGS += -DSYSMNGR_MEMORY_STATUS
endif

ifeq ($(SYSMNGR_PROCESS_STATUS),y)
OBJS += processes.o
PROG_CFLAGS += -DSYSMNGR_PROCESS_STATUS
endif

ifeq ($(SYSMNGR_SUPPORTED_DATA_MODEL),y)
OBJS += supported_dm.o
PROG_CFLAGS += -DSYSMNGR_SUPPORTED_DATA_MODEL
endif

ifeq ($(SYSMNGR_REBOOTS),y)
OBJS += reboots.o
PROG_CFLAGS += -DSYSMNGR_REBOOTS
endif

ifeq ($(SYSMNGR_NETWORK_PROPERTIES),y)
OBJS += network.o
PROG_CFLAGS += -DSYSMNGR_NETWORK_PROPERTIES
endif

ifeq ($(SYSMNGR_VENDOR_EXTENSIONS),y)
PROG_CFLAGS += -DSYSMNGR_VENDOR_EXTENSIONS
ifeq ($(filter -DCUSTOM_PREFIX=%,$(PROG_CFLAGS)),)
PROG_CFLAGS += -DCUSTOM_PREFIX=\"X_IOPSYS_EU_\"
endif
endif

ifneq ($(filter y,$(SYSMNGR_FIRMWARE_IMAGE) $(SYSMNGR_FWBANK_UBUS_SUPPORT)),)
OBJS += fwbank.o
endif

ifeq ($(SYSMNGR_FIRMWARE_IMAGE),y)
OBJS += fw_images.o
PROG_CFLAGS += -DSYSMNGR_FIRMWARE_IMAGE
endif

ifeq ($(SYSMNGR_FWBANK_UBUS_SUPPORT),y)
PROG_CFLAGS += -DSYSMNGR_FWBANK_UBUS_SUPPORT
endif

ifeq ($(SYSMNGR_TEMPERATURE_STATUS),y)
OBJS += temperature.o
PROG_CFLAGS += -DSYSMNGR_TEMPERATURE_STATUS
endif

.PHONY: all clean

%.o: %.c
	$(CC) $(PROG_CFLAGS) $(FPIC) -c -o $@ $<

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(PROG_CFLAGS) -o $@ $^ $(PROG_LDFLAGS)

clean:
	rm -f *.o $(PROG)
