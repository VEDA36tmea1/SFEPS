# RC522 유저 공간 테스트 (wiringPi + SPI)
# 필요: sudo apt install wiringpi  (또는 라즈베리 파이 wiringPi)
# 실행: sudo ./rc522_full_demo [--id] [--trailer N] [--write TEXT] ...

CC       = gcc
CFLAGS   = -Wall -O2
LDFLAGS  = -lwiringPi

TARGET   = rc522_full_demo
SRCS     = rc522_full_demo.c rc522_full.c
HEADERS  = rc522_full.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
