TARGET         ?= $(shell uname -r)
KERNEL_MODULES := /lib/modules/$(TARGET)
KERNEL_BUILD   := $(KERNEL_MODULES)/build
SYSTEM_MAP     := /boot/System.map-$(TARGET)
DRIVER         := asustor_as68xx
DRIVER_VERSION := 0.1.0

# DKMS
DKMS_ROOT_PATH = /usr/src/asustor-as68xx-$(DRIVER_VERSION)

asustor_as68xx_DEST_DIR = $(KERNEL_MODULES)/kernel/drivers/platform/x86

obj-m := $(DRIVER).o
# asustor_as68xx.ko bundles the platform entry point, MCU serial protocol
# and front-panel LCD framed protocol. The main source is named
# asustor_as68xx_main.c to avoid a circular self-reference with the module
# name (kbuild would otherwise silently drop it from the link).
asustor_as68xx-y := asustor_as68xx_main.o asustor_gpio.o asustor_mcu.o asustor_lcm.o

all: modules

modules:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) modules

install: modules_install

modules_install:
	install -m 644 -D $(DRIVER).ko $(asustor_as68xx_DEST_DIR)/$(DRIVER).ko
	depmod -a -F $(SYSTEM_MAP) $(TARGET)

clean:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) clean

.PHONY: all modules install modules_install clean dkms dkms_clean

dkms:
	@mkdir -p $(DKMS_ROOT_PATH)
	@echo "obj-m := asustor_as68xx.o" >$(DKMS_ROOT_PATH)/Makefile
	@echo "asustor_as68xx-y := asustor_as68xx_main.o asustor_gpio.o asustor_mcu.o asustor_lcm.o" >>$(DKMS_ROOT_PATH)/Makefile
	@cp dkms_as68xx.conf $(DKMS_ROOT_PATH)/dkms.conf
	@cp asustor_as68xx_main.c asustor_gpio.c asustor_gpio.h asustor_mcu.c asustor_mcu.h asustor_lcm.c asustor_lcm.h $(DKMS_ROOT_PATH)
	@sed -i -e '/^PACKAGE_VERSION=/ s/=.*/=\"$(DRIVER_VERSION)\"/' $(DKMS_ROOT_PATH)/dkms.conf

	@dkms add     -m asustor-as68xx -v $(DRIVER_VERSION)
	@dkms build   -m asustor-as68xx -v $(DRIVER_VERSION)
	@dkms install --force -m asustor-as68xx -v $(DRIVER_VERSION)
	@modprobe asustor_as68xx

dkms_clean:
	@rmmod asustor_as68xx 2> /dev/null || true
	@dkms remove  -m asustor-as68xx -v $(DRIVER_VERSION) --all
	@rm -rf $(DKMS_ROOT_PATH)
