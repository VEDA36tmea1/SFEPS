#ifndef ESP_PARSER_H
#define ESP_PARSER_H

#include <stdint.h>
#include <stddef.h>

/* ESP(+IPD payload)에서 들어오는 추적(track) 명령 파서
 *
 * Qt PwmTransmitter가 보내는 통일된 프로토콜:
 *   TRACK_START|<id>\n        → laser ON  + ACK
 *   SET_PWM,PAN=<us>,TILT=<us>\n → 서보 PAN/TILT 직접 설정
 *   TRACK_END|<id>\n          → laser OFF + ACK
 */

typedef void (*esp_laser_set_fn)(uint8_t on);             /* PB0 on/off */
typedef void (*esp_tcp_send_fn)(const char *payload);     /* TCP payload 전송 */
typedef void (*esp_dbg_tx_fn)(const char *msg);           /* USART2 디버그 출력 */
typedef void (*esp_servo_set_fn)(uint32_t pan_us,
                                  uint32_t tilt_us);      /* PAN(PA0/TIM2_CH1), TILT(PA8/TIM1_CH1) */

typedef struct
{
  esp_laser_set_fn  laser_set;   /* TRACK_START/END → 레이저 on/off */
  esp_tcp_send_fn   tcp_send;    /* ACK 전송 */
  esp_dbg_tx_fn     dbg_tx;      /* 시리얼 디버그 (NULL 허용) */
  esp_servo_set_fn  servo_set;   /* SET_PWM → 서보 직접 제어 (NULL 허용) */
} esp_parser_callbacks_t;

void    ESP_Parser_SetCallbacks(esp_parser_callbacks_t *cb);

/* +IPD payload 내부의 "한 줄"을 받아 처리한다.
 * 처리된 명령이면 1, 미처리(기존 EX/CX/숫자 PWM 등)는 0 반환 */
uint8_t ESP_Parser_HandleIpdLine(const char *line);

#endif /* ESP_PARSER_H */
