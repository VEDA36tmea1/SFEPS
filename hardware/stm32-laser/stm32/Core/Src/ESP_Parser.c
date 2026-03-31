#include "ESP_Parser.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static esp_laser_set_fn  s_laser_set = NULL;
static esp_tcp_send_fn   s_tcp_send  = NULL;
static esp_dbg_tx_fn     s_dbg_tx    = NULL;
static esp_servo_set_fn  s_servo_set = NULL;

static void trim_left(const char **p)
{
  while (*p && (**p == ' ' || **p == '\t'))
    (*p)++;
}

static void copy_until_stop(char *out, size_t out_sz, const char *p, char stop_char)
{
  if (!out || out_sz == 0)
    return;
  size_t i = 0;
  while (p && *p && *p != stop_char && *p != '\r' && *p != '\n')
  {
    if (i + 1 < out_sz)
      out[i++] = *p;
    p++;
  }
  out[i] = '\0';
}

static void copy_reason(char *out, size_t out_sz, const char *line)
{
  if (!out || out_sz == 0 || !line)
    return;
  out[0] = '\0';

  const char *r = strstr(line, "REASON=");
  if (!r)
    return;
  r += strlen("REASON=");
  copy_until_stop(out, out_sz, r, '|');
}

/* "KEY=<value>" 파싱 헬퍼: value를 정수(us)로 반환, 실패 시 def 반환 */
static uint32_t parse_key_uint(const char *line, const char *key, uint32_t def)
{
  const char *p = strstr(line, key);
  if (!p)
    return def;
  p += strlen(key);
  char *end = NULL;
  unsigned long v = strtoul(p, &end, 10);
  if (end == p)
    return def;
  return (uint32_t)v;
}

/* PWM 범위 클램프 (800~2200 us) */
#ifndef PWM_US_MIN
#define PWM_US_MIN 800u
#endif
#ifndef PWM_US_MAX
#define PWM_US_MAX 2200u
#endif

static uint32_t clamp_us(uint32_t v)
{
  if (v < PWM_US_MIN) return PWM_US_MIN;
  if (v > PWM_US_MAX) return PWM_US_MAX;
  return v;
}

void ESP_Parser_SetCallbacks(esp_parser_callbacks_t *cb)
{
  if (!cb)
    return;
  s_laser_set = cb->laser_set;
  s_tcp_send  = cb->tcp_send;
  s_dbg_tx    = cb->dbg_tx;
  s_servo_set = cb->servo_set;
}

uint8_t ESP_Parser_HandleIpdLine(const char *line)
{
  if (!line)
    return 0;

  const char *p = line;
  trim_left(&p);
  if (*p == '\0')
    return 0;

  char obj_id[64];
  char reason[96];

  /* ── TRACK_START|<object_id> ─────────────────────────────────────────── */
  if (strncmp(p, "TRACK_START|", 12) == 0)
  {
    const char *id_p = p + 12;
    copy_until_stop(obj_id, sizeof(obj_id), id_p, '|');

    if (obj_id[0] == '\0')
      return 1;

    if (s_laser_set)
      s_laser_set(1);

    if (s_tcp_send)
    {
      char resp[128];
      snprintf(resp, sizeof(resp), "TRACK_START_ACK|%s\n", obj_id);
      s_tcp_send(resp);
    }
    if (s_dbg_tx)
    {
      char dbg[160];
      snprintf(dbg, sizeof(dbg), "[TRACK] START id=%s → laser ON\r\n", obj_id);
      s_dbg_tx(dbg);
    }
    return 1;
  }

  /* ── SET_PWM,PAN=<us>,TILT=<us> ──────────────────────────────────────── */
  /* Qt PwmTransmitter 표준 포맷: "SET_PWM,PAN=1290,TILT=1390\n"           */
  if (strncmp(p, "SET_PWM", 7) == 0)
  {
    uint32_t pan  = parse_key_uint(p, "PAN=",  1500u);
    uint32_t tilt = parse_key_uint(p, "TILT=", 1500u);
    pan  = clamp_us(pan);
    tilt = clamp_us(tilt);

    if (s_servo_set)
      s_servo_set(pan, tilt);   /* PAN → PA0/TIM2_CH1, TILT → PA8/TIM1_CH1 */

    if (s_dbg_tx)
    {
      char dbg[80];
      snprintf(dbg, sizeof(dbg), "[PWM] PAN=%lu TILT=%lu us\r\n",
               (unsigned long)pan, (unsigned long)tilt);
      s_dbg_tx(dbg);
    }
    return 1;
  }

  /* ── TRACK_POS|<id>|... (매 프레임 수신, 처리 없음 — 인식만) ────────── */
  if (strncmp(p, "TRACK_POS|", 10) == 0)
  {
    return 1;
  }

  /* ── TRACK_END|<object_id>|REASON=<...> ──────────────────────────────── */
  if (strncmp(p, "TRACK_END|", 10) == 0)
  {
    const char *id_p = p + 10;
    copy_until_stop(obj_id, sizeof(obj_id), id_p, '|');
    copy_reason(reason, sizeof(reason), p);

    if (s_laser_set)
      s_laser_set(0);

    if (s_tcp_send)
    {
      char resp[160];
      if (reason[0] != '\0')
        snprintf(resp, sizeof(resp), "TRACK_END_ACK|%s|REASON=%s\n",
                 obj_id[0] ? obj_id : "UNKNOWN", reason);
      else
        snprintf(resp, sizeof(resp), "TRACK_END_ACK|%s\n",
                 obj_id[0] ? obj_id : "UNKNOWN");
      s_tcp_send(resp);
    }
    if (s_dbg_tx)
    {
      char dbg[200];
      if (reason[0] != '\0')
        snprintf(dbg, sizeof(dbg), "[TRACK] END id=%s REASON=%s → laser OFF\r\n",
                 obj_id[0] ? obj_id : "UNKNOWN", reason);
      else
        snprintf(dbg, sizeof(dbg), "[TRACK] END id=%s → laser OFF\r\n",
                 obj_id[0] ? obj_id : "UNKNOWN");
      s_dbg_tx(dbg);
    }
    return 1;
  }

  return 0;
}
