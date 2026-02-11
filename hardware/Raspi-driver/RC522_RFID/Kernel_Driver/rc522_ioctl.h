/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RC522 user/kernel IOCTL API
 * 유저 공간 테스트 앱에서 #include "rc522_ioctl.h" 후 사용
 */

#ifndef _RC522_IOCTL_H_
#define _RC522_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

/* Magic number: 'R' (RC522) */
#define RC522_IOC_MAGIC  'R'

/* 리더기 소프트 리셋 (인자 없음) */
#define RC522_RESET      _IO(RC522_IOC_MAGIC, 0)

/* 카드 감지 및 UID 읽기. 블로킹. 인자: __u32 포인터 → UID 4바이트 저장 */
#define RC522_READ_CARD  _IOR(RC522_IOC_MAGIC, 1, __u32)

/* 레지스터 1바이트 읽기. 인자: struct rc522_reg_data (reg 입력, val 출력) */
#define RC522_READ_REG   _IOWR(RC522_IOC_MAGIC, 2, struct rc522_reg_data)

/* 레지스터 1바이트 쓰기. 인자: struct rc522_reg_data (reg, val 입력) */
#define RC522_WRITE_REG  _IOW(RC522_IOC_MAGIC, 3, struct rc522_reg_data)

/* 섹터 텍스트 읽기. 인자: struct rc522_read_text (trailer_block 입력, uid/text 출력) */
#define RC522_READ_TEXT_SECTOR  _IOWR(RC522_IOC_MAGIC, 4, struct rc522_read_text)

struct rc522_reg_data {
	__u8 reg;
	__u8 val;
};

struct rc522_read_text {
	__s32 trailer_block;  /* 입력: 섹터 트레일러 블록 (예: 11, 15, 19...) */
	__u32 uid;            /* 출력: UID */
	char text[48];        /* 출력: 텍스트 (널 종료) */
};

/*
 * 유저 공간 사용 예 (테스트 앱):
 *   #include "rc522_ioctl.h"
 *   int fd = open("/dev/rc522", O_RDWR);
 *   uint32_t uid;
 *   ioctl(fd, RC522_READ_CARD, &uid);   // 블로킹 UID 읽기
 *   ioctl(fd, RC522_RESET);             // 소프트 리셋
 *   struct rc522_reg_data r = { .reg = 0x37 };  // VersionReg
 *   ioctl(fd, RC522_READ_REG, &r);      // r.val 에 값 반환
 */

#endif /* _RC522_IOCTL_H_ */
