ifeq ($(KERNELRELEASE),)

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: build clean

build:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.0 *~ core .depend .*.cmd *.ko *.mod.c *.o
	rm -f modules.order Module.symvers

else

$(info Building with KERNELRELEASE = ${KERNELRELEASE})
obj-m :=	rrpc_debug.o

endif

