# SPDX-License-Identifier: GPL-2.0
# Kernel Computation Reuse (KCR) Module Makefile

ccflags-y := -DKCR_DEBUG -DCONFIG_KCR

obj-m += kcr.o
kcr-objs := kcr_main.o kcr_mem.o kcr_cache.o kcr_debugfs.o \
	    ../../kernel/kcr/kcr_core.o ../../kernel/kcr/kcr_determinism.o \
	    ../iommu/kcr_iommu.o

ccflags-y += -I$(src)/../../include

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod *.order *.symvers .*.cmd

install:
	install -D kcr.ko /lib/modules/$(shell uname -r)/kernel/drivers/kcr/
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/kernel/drivers/kcr/kcr.ko
	depmod -a

load:
	insmod kcr.ko

unload:
	rmmod kcr

.PHONY: all clean install uninstall load unload
