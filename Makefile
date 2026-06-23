KDIR=/lib/modules/`uname -r`/build
obj-m := f81601a.o

all: module

module:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: Kernel headers not found at $(KDIR)" >&2; \
		echo "" >&2; \
		echo "Install the matching kernel headers package, for example:" >&2; \
		echo "  Ubuntu/Debian:  apt install linux-headers-`uname -r`" >&2; \
		echo "  Rockchip:       apt install linux-headers-current-rockchip-rk3588" >&2; \
		echo "  Or search:      apt search linux-headers | grep `uname -r`" >&2; \
		exit 1; \
	fi
	make -C $(KDIR) M=$(PWD) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		make -C $(KDIR) M=$(PWD) clean; \
	fi

.PHONY: all module clean
