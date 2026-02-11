// rc522_full.h
// Python mfrc522(SimpleMFRC522/BasicMFRC522/MFRC522) 흐름을 C로 포팅한 API
// - wiringPi + spidev(/dev/spidevX.Y) 기반
//
// 사용 예:
//   #include "rc522_full.h"
//   rc522c_init(0, 1000000, 25);
//   uint32_t id;
//   rc522c_read_id_blocking(&id);

#ifndef RC522_FULL_H
#define RC522_FULL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// SPI 및 RC522 초기화
// - spi_ch: 0 -> /dev/spidev0.0(CE0), 1 -> /dev/spidev0.1(CE1)
// - speed_hz: SPI clock (예: 1000000)
// - rst_bcm: RC522 RST에 연결된 BCM GPIO 번호 (예: 25). RST를 3.3V에 고정한 경우 -1 권장.
// 반환: 0 성공, -1 실패
int rc522c_init(int spi_ch, int speed_hz, int rst_bcm);

// UID를 한 번만 시도해 읽기 (태그 없으면 실패)
// 반환: 0 성공, -1 실패
int rc522c_read_id_no_block(uint32_t *out_id);

// UID를 태그가 나올 때까지 blocking으로 읽기
// 반환: 0 성공(실패하지 않도록 설계)
int rc522c_read_id_blocking(uint32_t *out_id);

// 섹터(트레일러 블록 기준)의 3개 데이터 블록을 ASCII 텍스트로 읽기
// - trailer_block: 예) 11, 15, 19 ... ((trailer_block+1)%4==0 인 값)
// - out_text: 널 종료 문자열로 채움
// - max_len: out_text 버퍼 크기
// 반환: 0 성공, -1 실패
int rc522c_read_text_sector_blocking(int trailer_block,
                                     uint32_t *out_id,
                                     char *out_text,
                                     size_t max_len);

// 섹터(트레일러 블록 기준)의 3개 데이터 블록에 텍스트 쓰기 (최대 48바이트, 부족하면 0으로 패딩)
// 반환: 0 성공, -1 실패
int rc522c_write_text_sector_blocking(int trailer_block,
                                      const char *text,
                                      uint32_t *out_id);

#ifdef __cplusplus
}
#endif

#endif // RC522_FULL_H

