/*
 * RC522 커널 드라이버 테스트
 * - read() 또는 ioctl(RC522_READ_CARD) 로 UID 블로킹 읽기
 * - ioctl(RC522_READ_REG) 로 VersionReg(0x37) 읽기
 *
 * 빌드: make test_rc522  또는  gcc -o test_rc522 test_rc522.c -I.
 * 실행: sudo ./test_rc522
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rc522_ioctl.h"

#define DEV_RC522 "/dev/rc522"

int main(void)
{
	int fd;
	uint32_t uid;
	uint8_t buf[4];
	ssize_t n;
	struct rc522_reg_data reg;

	fd = open(DEV_RC522, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_RC522);
		fprintf(stderr, "드라이버 로드 및 디바이스 트리 오버레이 확인: lsmod | grep rc522, ls -l /dev/rc522\n");
		return 1;
	}

	printf("=== RC522 드라이버 테스트 ===\n\n");

	/* VersionReg(0x37) 읽기 - 칩 연결 확인 */
	reg.reg = 0x37;
	reg.val = 0;
	if (ioctl(fd, RC522_READ_REG, &reg) < 0) {
		perror("ioctl RC522_READ_REG");
		close(fd);
		return 1;
	}
	printf("[1] VersionReg(0x37) = 0x%02X (정상: 0x91 또는 0x92)\n\n", reg.val);

	/* read() 로 UID 읽기 (태그가 올 때까지 대기). 중단: Ctrl+C */
	printf("[2] read() 로 UID 대기 중... (태그를 리더에 갖다 대세요, 종료: Ctrl+C)\n");
	n = read(fd, buf, 4);
	if (n != 4) {
		if (n < 0 && errno == EINTR) {
			printf("\n중단됨 (Ctrl+C)\n");
			close(fd);
			return 0;
		}
		perror("read");
		close(fd);
		return 1;
	}
	uid = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
	      (uint32_t)buf[2] << 8 | (uint32_t)buf[3];
	printf("     UID (read): %08X\n\n", (unsigned)uid);

	/* ioctl 로 한 번 더 UID 읽기 */
	printf("[3] ioctl(RC522_READ_CARD) 로 UID 대기 중... (태그를 다시 갖다 대세요, 종료: Ctrl+C)\n");
	if (ioctl(fd, RC522_READ_CARD, &uid) < 0) {
		if (errno == EINTR) {
			printf("\n중단됨 (Ctrl+C)\n");
			close(fd);
			return 0;
		}
		perror("ioctl RC522_READ_CARD");
		close(fd);
		return 1;
	}
	printf("     UID (ioctl): %08X\n\n", (unsigned)uid);

	printf("테스트 완료.\n");
	close(fd);
	return 0;
}
