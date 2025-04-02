# SPDX-License-Identifier: GPL-2.0
obj-m := gpu_ptr_translator.o

gpu_ptr_translator-y := main.o

gpu_ptr_translator.ko: main.c
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

install: gpu_ptr_translator.ko
	sudo insmod $^
	lsmod | grep gpu_ptr_translator
	ls -l /dev/gpu_ptr_translator

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	sudo rmmod gpu_ptr_translator

.PHONY: install clean
