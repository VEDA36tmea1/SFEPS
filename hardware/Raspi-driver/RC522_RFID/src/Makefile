CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11
LDFLAGS := -lwiringPi

TARGET_TEST := rc522_test
SRC_TEST := rc522_test.c

TARGET_FULL := rc522_full_demo
SRC_FULL := rc522_full_demo.c rc522_full.c

.PHONY: all clean run run-full check

all: check $(TARGET_TEST) $(TARGET_FULL)

check:
	@if command -v rg >/dev/null 2>&1; then \
		rg -n "^[[:space:]]*int[[:space:]]+main[[:space:]]*\\(" "$(SRC_TEST)" >/dev/null; \
	else \
		grep -nE "^[[:space:]]*int[[:space:]]+main[[:space:]]*\\(" "$(SRC_TEST)" >/dev/null; \
	fi || ( echo "ERROR: $(SRC_TEST) 에 main() 함수가 없습니다 (동기화/파일내용 확인 필요)"; exit 1 )

$(TARGET_TEST): $(SRC_TEST)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(TARGET_FULL): $(SRC_FULL)
	$(CC) $(CFLAGS) -o $@ $(SRC_FULL) $(LDFLAGS)

run: $(TARGET_TEST)
	sudo ./$(TARGET_TEST)

run-full: $(TARGET_FULL)
	sudo ./$(TARGET_FULL)

clean:
	rm -f $(TARGET_TEST) $(TARGET_FULL)
