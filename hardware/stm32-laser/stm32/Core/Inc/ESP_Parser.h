#ifndef ESP_PARSER_H
#define ESP_PARSER_H

#include <stdint.h>
#include <stddef.h>

/* ESP(+IPD payload)에서 들어오는 추적(track) 명령 파서 */

typedef void (*esp_laser_set_fn)(uint8_t on);               /* PB0 on/off */
typedef void (*esp_tcp_send_fn)(const char *payload);    /* TCP payload 라인 전송(개행 포함 권장) */
typedef void (*esp_dbg_tx_fn)(const char *msg);          /* PC(USART2) 디버그 출력 (NULL이면 생략) */

typedef struct
{
  esp_laser_set_fn laser_set;
  esp_tcp_send_fn tcp_send;
  esp_dbg_tx_fn dbg_tx; /* TRACK_* 수신 시 시리얼 모니터에 한 줄 요약 */
} esp_parser_callbacks_t;

void ESP_Parser_SetCallbacks(esp_parser_callbacks_t *cb);

/* +IPD payload 내부의 "한 줄"을 받아 처리한다.
 * - TRACK_START/POS/END를 처리하면 1을 반환
 * - 그 외(EX/CX/숫자 PWM 등)는 0 반환 (기존 main.c 로직에서 처리) */
uint8_t ESP_Parser_HandleIpdLine(const char *line);

#endif /* ESP_PARSER_H */

