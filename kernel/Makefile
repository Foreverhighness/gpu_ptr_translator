# SPDX-License-Identifier: GPL-2.0
obj-m := gpu_ptr_translator.o

gpu_ptr_translator-y := main.o

# Note: Uses KDIR environment variable if set, otherwise defaults to running kernel build path
KDIR ?= /lib/modules/$(shell uname -r)/build

gpu_ptr_translator.ko: main.c gpu_ptr_translator.h amd_rdma.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: gpu_ptr_translator.ko
	@echo "Attempting to unload module if loaded..."
	-sudo rmmod gpu_ptr_translator || true
	@echo "Installing module..."
	sudo insmod $<
	@echo "Verifying installation..."
	lsmod | grep gpu_ptr_translator
	ls -l /dev/gpu_ptr_translator
	dmesg | tail -n 5

load: gpu_ptr_translator.ko
	sudo insmod $^
	lsmod | grep gpu_ptr_translator
	ls -l /dev/gpu_ptr_translator

unload:
	sudo rmmod gpu_ptr_translator

reload: unload load

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	sudo rmmod gpu_ptr_translator

.PHONY: install clean load unload reload
