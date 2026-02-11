/*
 * RC522 지속 폴링 테스트 - 다른 카드 대면 다른 값 출력
 * - 계속 폴링하며 UID와 텍스트 읽기
 * - UID가 바뀌면 출력
 *
 * 빌드: make test_rc522_poll  또는  gcc -o test_rc522_poll test_rc522_poll.c -I.
 * 실행: sudo ./test_rc522_poll [--trailer N]
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rc522_ioctl.h"

#define DEV_RC522 "/dev/rc522"

static volatile int running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	/* Ctrl+C로 블로킹 ioctl/read가 즉시 EINTR로 깨도록 SA_RESTART 사용 안 함 */
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) < 0)
		return -1;
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	uint32_t last_uid = 0;
	uint32_t uid;
	int trailer = 11;  /* 기본 섹터 11 (블록 8-10) */
	struct rc522_read_text text_data;

	if (argc > 2 && !strcmp(argv[1], "--trailer")) {
		trailer = atoi(argv[2]);
		if (trailer < 0 || (trailer + 1) % 4 != 0) {
			fprintf(stderr, "trailer는 11, 15, 19, ... (4의 배수-1) 이어야 함\n");
			return 1;
		}
	}

	fd = open(DEV_RC522, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_RC522);
		return 1;
	}

	if (install_signal_handlers() < 0) {
		perror("sigaction");
		close(fd);
		return 1;
	}

	printf("=== RC522 지속 폴링 테스트 ===\n");
	printf("섹터 트레일러 블록: %d (블록 %d-%d)\n", trailer, trailer - 3, trailer - 1);
	printf("카드를 리더에 갖다 대세요. 종료: Ctrl+C\n\n");

	while (running) {
		/* UID 읽기 */
		if (ioctl(fd, RC522_READ_CARD, &uid) < 0) {
			if (errno == EINTR) {
				printf("\n중단됨 (Ctrl+C)\n");
				break;
			}
			usleep(100000);  /* 100ms 대기 후 재시도 */
			continue;
		}

		/* UID가 바뀌었을 때만 출력 */
		if (uid != last_uid) {
			printf("\n[새 카드 감지] UID: %08X\n", (unsigned)uid);

			/* 섹터 텍스트 읽기 */
			memset(&text_data, 0, sizeof(text_data));
			text_data.trailer_block = trailer;
			if (ioctl(fd, RC522_READ_TEXT_SECTOR, &text_data) == 0) {
				printf("  UID: %08X\n", (unsigned)text_data.uid);
				printf("  텍스트: \"%s\"\n", text_data.text);
			} else {
				printf("  텍스트 읽기 실패 (인증 실패 또는 빈 섹터)\n");
			}
			printf("\n");

			last_uid = uid;
		}

		usleep(200000);  /* 200ms 간격으로 폴링 */
	}

	close(fd);
	return 0;
}
