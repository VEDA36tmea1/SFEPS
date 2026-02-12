# RC522 IRQ 전환 실험용 Makefile
# - 원본 Kernel_Driver는 유지
# - 이 폴더에서 파일이 없으면 ../Kernel_Driver 소스를 참조

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(CURDIR)/build
SRC_BASE  ?= ../Kernel_Driver

MOD_SRCS := rc522_core.c rc522_spi.c rc522_chardev.c
MOD_HDRS := rc522.h rc522_ioctl.h

.PHONY: all clean prepare_build dtbo

all: prepare_build
	$(MAKE) -C $(KERNEL_SRC) M=$(BUILD_DIR) modules

prepare_build: $(BUILD_DIR)/Makefile
	@for f in $(MOD_SRCS) $(MOD_HDRS); do \
		if [ -f "$(CURDIR)/$$f" ]; then \
			ln -sf "$(CURDIR)/$$f" "$(BUILD_DIR)/$$f"; \
		else \
			ln -sf "$(abspath $(SRC_BASE))/$$f" "$(BUILD_DIR)/$$f"; \
		fi; \
	done

$(BUILD_DIR)/Makefile:
	@mkdir -p $(BUILD_DIR)
	@echo 'obj-m := rc522.o' > $(BUILD_DIR)/Makefile
	@echo 'rc522-y := rc522_core.o rc522_spi.o rc522_chardev.o' >> $(BUILD_DIR)/Makefile

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(BUILD_DIR) clean

dtbo: rc522-overlay-irq.dtbo

rc522-overlay-irq.dtbo: rc522-overlay-irq.dts
	dtc -@ -I dts -O dtb -o rc522-overlay-irq.dtbo rc522-overlay-irq.dts

