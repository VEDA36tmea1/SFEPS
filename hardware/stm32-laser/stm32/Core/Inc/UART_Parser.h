#ifndef UART_PARSER_H
#define UART_PARSER_H

#include <stdint.h>
#include <stddef.h>

typedef void (*uart_send_to_esp_fn)(const char *at_line);
typedef void (*uart_tx_pc_fn)(const char *msg);

typedef void (*uart_set_mode_fn)(uint8_t mode); /* MODE_MANUAL=0, MODE_AUTO=1 */
typedef void (*uart_servo_set_all_fn)(uint32_t us_ch1, uint32_t us_ch2);
typedef void (*uart_servo_set_ch1_fn)(uint32_t us_ch1); /* PA8 / TIM1_CH1 */
typedef void (*uart_servo_set_ch2_fn)(uint32_t us_ch2); /* PA0 / TIM2_CH1 */
typedef void (*uart_notify_last_tick_fn)(void);
typedef void (*uart_set_auto_pwm_fn)(uint32_t us_ch1);
typedef void (*uart_notify_at_sent_fn)(void);

typedef struct
{
  uint32_t pwm_min_us;
  uint32_t pwm_max_us;

  uart_send_to_esp_fn send_to_esp;
  uart_tx_pc_fn tx_pc;

  uart_set_mode_fn set_mode;
  uart_servo_set_all_fn set_servo_all;
  uart_servo_set_ch1_fn set_servo_ch1;
  uart_servo_set_ch2_fn set_servo_ch2;

  uart_notify_last_tick_fn notify_last_tick;
  uart_set_auto_pwm_fn set_auto_pwm_val;
  uart_notify_at_sent_fn notify_at_sent;
} uart_parser_callbacks_t;

void UART_Parser_SetCallbacks(uart_parser_callbacks_t *cb);
void UART_Parser_HandleLine(const char *line);

#endif /* UART_PARSER_H */

