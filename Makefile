  # You will want to fix this line
KBUILD_EXTRA_SYMBOLS := /home/madcat/Devel/KLGD/Module.symvers
KBUILD_CFLAGS += -g3

ifneq ($(KERNELRELEASE),)
  	klgdffm-y := klgd_ff_plugin.o
  	klgdffm-y += klgdff.o
	obj-m += klgdffm.o

else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

endif
