EXTRA_CFLAGS = -g -O2
KVERSION ?= $(shell uname -r)
PWD ?= $(shell pwd)
obj-m += f81601a.o
ccflags-y=-I/usr/lib/gcc/x86_64-linux-gnu/7/include

all: default

default:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
	#make -C /DataDisk/freescale/imx/build/tmp/work/imx6qsabresd-poky-linux-gnueabi/linux-imx/4.9.11-r0/build  M=$(PWD) modules
	#make -C /DataDisk/old/hpeter/allwinner/loboris/test/sunxi_jwrdegoede  M=$(PWD) modules
	#make -C /DataDisk/hpeter/DMA-210UII/samsung_android_kernel_3.0 M=$(PWD) modules
	#make -C /DataDisk/freescale/android_x86-6.0-r2/out/target/product/x86/obj/kernel M=$(PWD) modules
	#make -C /DataDisk/freescale/x86/android_lolipop/out/target/product/x86/obj/kernel M=$(PWD) modules

install:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) INSTALL_MOD_DIR=updates modules_install
	depmod

clean:
	#make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
	rm -rf *.*~ *~ *.o *.ko *.mod.c .*.cmd .*.o.d .tmp_versions Module.symvers modules.order Module.markers
