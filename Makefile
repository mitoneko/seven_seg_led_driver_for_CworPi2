CFILES = seven_seg.c

obj-m := seven_seg.o
gpioModule-objs := $(CFILES:.c=.o)
ccflags-y += -std=gnu99 -Wall -Wno-declaration-after-statement

PWD := $(shell pwd)

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
