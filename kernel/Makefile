obj-m := touch_interceptor.o
touch_interceptor-objs := main.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# 默认用 clang，Android 内核用
CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH ?= arm64

# 如果有 CC 设置就用，否则用默认
ifdef CC
  EXTRA_CFLAGS += -DCC_SET
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	adb push touch_interceptor.ko /data/local/tmp/
	adb shell su -c "insmod /data/local/tmp/touch_interceptor.ko"

uninstall:
	adb shell su -c "rmmod touch_interceptor"

log:
	adb shell su -c "dmesg | grep touch_interceptor | tail -20"

.PHONY: all clean install uninstall log
