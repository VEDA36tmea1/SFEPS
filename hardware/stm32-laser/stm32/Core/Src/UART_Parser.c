#include "UART_Parser.h"

#include <string.h>
#include <stdio.h>

static uart_parser_callbacks_t s_cb;

static void trim_left(const char **p)
{
  while (*p && (**p == ' ' || **p == '\t'))
    (*p)++;
}

void UART_Parser_SetCallbacks(uart_parser_callbacks_t *cb)
{
  if (!cb)
    return;
  s_cb = *cb;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

void UART_Parser_HandleLine(const char *line)
{
  if (!line)
    return;

  const char *p = line;
  trim_left(&p);
  if (*p == '\0' || *p == '\r' || *p == '\n')
    return;

  /* ESP: 프리픽스 패스스루 */
  if (strncmp(p, "ESP:", 4) == 0 || strncmp(p, "esp:", 4) == 0)
  {
    const char *cmd = p + 4;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\0')
      return;

    if (s_cb.send_to_esp)
      s_cb.send_to_esp(cmd);

    if (s_cb.tx_pc)
    {
      char out[160];
      /* main.c 기존 출력(ESP: + cmd + CRLF)을 최대한 비슷하게 유지 */
      snprintf(out, sizeof(out), "ESP:%s\r\n", cmd);
      s_cb.tx_pc(out);
    }
    return;
  }

  /* AT 패스스루 */
  if (strncmp(p, "AT", 2) == 0 || strncmp(p, "at", 2) == 0)
  {
    if (s_cb.send_to_esp)
      s_cb.send_to_esp(p);

    if (s_cb.tx_pc)
    {
      char out[140];
      snprintf(out, sizeof(out), "%s\r\n", p);
      s_cb.tx_pc(out);
    }

    if (s_cb.notify_at_sent)
      s_cb.notify_at_sent();
    return;
  }

  /* mode 0 / mode 1 */
  if (strncmp(p, "mode", 4) == 0)
  {
    unsigned long m = 0;
    if (sscanf(p + 4, "%lu", &m) == 1 && (m == 0ul || m == 1ul))
    {
      if (s_cb.set_mode)
        s_cb.set_mode((uint8_t)m);

      if (s_cb.tx_pc)
      {
        char out[80];
        if (m == 0ul)
          snprintf(out, sizeof(out), "MODE=0 (manual)\r\n");
        else
          snprintf(out, sizeof(out), "MODE=1 (auto sweep 1200~1800us)\r\n");
        s_cb.tx_pc(out);
      }
    }
    else
    {
      if (s_cb.tx_pc)
        s_cb.tx_pc("Usage: mode 0 (manual) or mode 1 (auto)\r\n");
    }
    return;
  }

  /* 기본: 서보 제어 명령
   * - "1500" 또는 "1500 1200"  → CH1=PA8(TIM1), CH2=PA0(TIM2)
   * - "X:1500" 또는 "X 1500"   → CH2만 변경
   * - "Y:1200" 또는 "Y 1200"   → CH1만 변경
   */
  char c0 = *p;
  if (c0 == 'X' || c0 == 'x')
  {
    p++;
    if (*p == ':' || *p == ' ')
      p++;
    unsigned long ux = 1500;
    if (sscanf(p, "%lu", &ux) == 1)
    {
      ux = clamp_u32((uint32_t)ux, s_cb.pwm_min_us, s_cb.pwm_max_us);
      if (s_cb.set_servo_ch2)
        s_cb.set_servo_ch2((uint32_t)ux);
      if (s_cb.notify_last_tick)
        s_cb.notify_last_tick();

      if (s_cb.tx_pc)
      {
        char out[90];
        snprintf(out, sizeof(out), "\nOK X=PA0=%lu us\r\n", ux);
        s_cb.tx_pc(out);
      }
    }
    else
    {
      if (s_cb.tx_pc)
        s_cb.tx_pc("? Usage: X:1500\r\n");
    }
    return;
  }
  else if (c0 == 'Y' || c0 == 'y')
  {
    p++;
    if (*p == ':' || *p == ' ')
      p++;
    unsigned long uy = 1500;
    if (sscanf(p, "%lu", &uy) == 1)
    {
      uy = clamp_u32((uint32_t)uy, s_cb.pwm_min_us, s_cb.pwm_max_us);
      if (s_cb.set_servo_ch1)
        s_cb.set_servo_ch1((uint32_t)uy);
      if (s_cb.set_auto_pwm_val)
        s_cb.set_auto_pwm_val((uint32_t)uy);
      if (s_cb.notify_last_tick)
        s_cb.notify_last_tick();

      if (s_cb.tx_pc)
      {
        char out[90];
        snprintf(out, sizeof(out), "\nOK Y=PA8=%lu us\r\n", uy);
        s_cb.tx_pc(out);
      }
    }
    else
    {
      if (s_cb.tx_pc)
        s_cb.tx_pc("? Usage: Y:1500\r\n");
    }
    return;
  }

  /* else: "1500" 또는 "1500 1200" */
  unsigned long u1 = 1500, u2 = 1500;
  int n = sscanf(line, "%lu %lu", &u1, &u2);
  if (n >= 1)
  {
    u1 = clamp_u32((uint32_t)u1, s_cb.pwm_min_us, s_cb.pwm_max_us);
    if (n >= 2)
      u2 = clamp_u32((uint32_t)u2, s_cb.pwm_min_us, s_cb.pwm_max_us);
    else
      u2 = u1;

    if (s_cb.set_servo_all)
      s_cb.set_servo_all((uint32_t)u1, (uint32_t)u2);
    if (s_cb.set_auto_pwm_val)
      s_cb.set_auto_pwm_val((uint32_t)u1);
    if (s_cb.notify_last_tick)
      s_cb.notify_last_tick();

    if (s_cb.tx_pc)
    {
      char out[110];
      snprintf(out, sizeof(out), "\nOK PA8=%lu PA0=%lu us\r\n", u1, u2);
      s_cb.tx_pc(out);
    }
  }
  else
  {
    if (s_cb.tx_pc)
      s_cb.tx_pc("? (send: 1500 or 1500 1200, X:1500, Y:1500, or mode 0/1)\r\n");
  }
}

