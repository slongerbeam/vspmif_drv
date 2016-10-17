CFILES = vspm_if_main.c vspm_if_sub.c

obj-m += vspm_if.o
vspm_if-objs := $(CFILES:.c=.o)

U_INCLUDE := -I$(PWD)
U_INCLUDE += -I$(PWD)/../include
U_INCLUDE += -I$(KERNELSRC)/include
EXTRA_CFLAGS += $(U_INCLUDE)

all:
	make -C $(KERNELSRC) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(KERNELSRC)/include/vspm.symvers modules
clean:
	make -C $(KERNELSRC) M=$(PWD) clean
install:
	$(CP) ../include/vspm_if.h $(KERNELSRC)/include

