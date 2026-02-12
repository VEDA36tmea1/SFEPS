# RC522 (MFRC522) kernel driver - out-of-tree build
# 빌드 결과물은 build/ 에 생성됨.
# Usage (on Raspberry Pi):
#   make
#   sudo insmod build/rc522.ko
#
# Cross-compile (optional):
#   make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KERNEL_SRC=/path/to/linux

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
ARCH      ?= $(shell uname -m)
BUILD_DIR := $(CURDIR)/build
MOD_SRCS  := rc522_core.c rc522_spi.c rc522_chardev.c
MOD_HDRS  := rc522.h rc522_ioctl.h

.PHONY: all clean modules_install dtbo prepare_build

all: prepare_build
	$(MAKE) -C $(KERNEL_SRC) M=$(BUILD_DIR) modules

prepare_build: $(BUILD_DIR)/Makefile
	@for f in $(MOD_SRCS) $(MOD_HDRS); do \
		ln -sf ../$$f $(BUILD_DIR)/$$f 2>/dev/null || true; \
	done

$(BUILD_DIR)/Makefile:
	@mkdir -p $(BUILD_DIR)
	@echo 'obj-m := rc522.o' > $(BUILD_DIR)/Makefile
	@echo 'rc522-y := rc522_core.o rc522_spi.o rc522_chardev.o' >> $(BUILD_DIR)/Makefile

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(BUILD_DIR) clean
	@rm -f Module.symvers modules.order rc522.ko rc522.mod rc522.mod.c rc522.o rc522_core.o rc522_spi.o rc522_chardev.o
	@rm -f .*.cmd .*.o.cmd .*.ko.cmd .Module.symvers.cmd .modules.order.cmd
	@echo "Cleaned build artifacts from source directory"

modules_install: all
	$(MAKE) -C $(KERNEL_SRC) M=$(BUILD_DIR) modules_install

# Device tree overlay (compile .dts -> .dtbo on the Pi or with dtc)
dtbo: rc522-overlay.dtbo rc522-overlay-nospidev.dtbo

rc522-overlay.dtbo: rc522-overlay.dts
	dtc -@ -I dts -O dtb -o rc522-overlay.dtbo rc522-overlay.dts

# spidev 비활성화 없는 버전 (오버레이 적용 실패 시 시도)
rc522-overlay-nospidev.dtbo: rc522-overlay-nospidev.dts
	dtc -@ -I dts -O dtb -o rc522-overlay-nospidev.dtbo rc522-overlay-nospidev.dts

install-dtbo: rc522-overlay.dtbo
	@echo "Copy rc522-overlay.dtbo to /boot/overlays/ and add to /boot/config.txt:"
	@echo "  dtoverlay=rc522-overlay"
	@echo "Then reboot."
	sudo cp -i rc522-overlay.dtbo /boot/overlays/ 2>/dev/null || true

# 유저 공간 테스트 (드라이버 로드 후 sudo ./test_rc522)
test_rc522: test_rc522.c rc522_ioctl.h
	gcc -Wall -O2 -o test_rc522 test_rc522.c -I.

# 지속 폴링 테스트 (다른 카드 대면 다른 값 출력)
test_rc522_poll: test_rc522_poll.c rc522_ioctl.h
	gcc -Wall -O2 -o test_rc522_poll test_rc522_poll.c -I.
