// rc522_full_demo.c
// Python SimpleMFRC522처럼: UID + 텍스트(섹터) 읽기/쓰기 데모
//
// 빌드: Makefile의 rc522_full_demo 타깃 사용
// 실행 예:
//   ./rc522_full_demo                 # trailer=11 읽기
//   ./rc522_full_demo --id            # UID만 출력
//   ./rc522_full_demo --trailer 11    # trailer 지정
//   ./rc522_full_demo --write "hello" # trailer=11에 쓰기
//   ./rc522_full_demo --ch 0 --speed 1000000 --rst 25

#include "rc522_full.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    printf("Usage: %s [--id] [--trailer N] [--write TEXT] [--ch 0|1] [--speed HZ] [--rst BCM|-1]\n", argv0);
    printf("  --id           UID만 출력\n");
    printf("  --trailer N    섹터 트레일러 블록(기본 11)\n");
    printf("  --write TEXT   지정 trailer 섹터에 TEXT 쓰기(최대 48바이트)\n");
    printf("  --ch 0|1       SPI CS: 0(/dev/spidev0.0), 1(/dev/spidev0.1)\n");
    printf("  --speed HZ     SPI 속도(기본 1000000)\n");
    printf("  --rst BCM|-1   RST GPIO(기본 25). RST를 3.3V 고정이면 -1 추천\n");
}

int main(int argc, char **argv)
{
    int spi_ch = 0;
    int speed_hz = 1000000;
    int rst_bcm = 25;
    int trailer = 11;
    int only_id = 0;
    const char *write_text = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--id")) {
            only_id = 1;
            continue;
        }
        if (!strcmp(argv[i], "--trailer")) {
            if (i + 1 >= argc) return (usage(argv[0]), 2);
            trailer = (int)strtol(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--write")) {
            if (i + 1 >= argc) return (usage(argv[0]), 2);
            write_text = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--ch")) {
            if (i + 1 >= argc) return (usage(argv[0]), 2);
            spi_ch = (int)strtol(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--speed")) {
            if (i + 1 >= argc) return (usage(argv[0]), 2);
            speed_hz = (int)strtol(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--rst")) {
            if (i + 1 >= argc) return (usage(argv[0]), 2);
            rst_bcm = (int)strtol(argv[++i], NULL, 10);
            continue;
        }

        fprintf(stderr, "Unknown arg: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    if (rc522c_init(spi_ch, speed_hz, rst_bcm) != 0) {
        fprintf(stderr, "rc522 init failed (ch=%d speed=%d rst=%d)\n", spi_ch, speed_hz, rst_bcm);
        return 1;
    }

    if (only_id) {
        uint32_t id;
        rc522c_read_id_blocking(&id);
        printf("%u\n", id);
        return 0;
    }

    if (write_text) {
        uint32_t id;
        if (rc522c_write_text_sector_blocking(trailer, write_text, &id) != 0) {
            fprintf(stderr, "write failed\n");
            return 1;
        }
        printf("ID: %u\n", id);
        printf("WROTE: %s\n", write_text);
        return 0;
    }

    {
        uint32_t id;
        char text[64];
        if (rc522c_read_text_sector_blocking(trailer, &id, text, sizeof(text)) != 0) {
            fprintf(stderr, "read failed\n");
            return 1;
        }
        printf("ID: %u\n", id);
        printf("TEXT: %s\n", text);
    }

    return 0;
}

