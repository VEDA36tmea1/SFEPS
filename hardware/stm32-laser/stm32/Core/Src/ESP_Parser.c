#include "ESP_Parser.h"

#include <string.h>
#include <stdio.h>

static esp_laser_set_fn s_laser_set = NULL;
static esp_tcp_send_fn s_tcp_send = NULL;

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

  /* REASON은 보통 '|' 앞까지 */
  copy_until_stop(out, out_sz, r, '|');
}

void ESP_Parser_SetCallbacks(esp_parser_callbacks_t *cb)
{
  if (!cb)
    return;
  s_laser_set = cb->laser_set;
  s_tcp_send = cb->tcp_send;
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

  /* TRACK_START|<object_id> */
  /* "TRACK_START|" length = 12 */
  if (strncmp(p, "TRACK_START|", 12) == 0)
  {
    const char *id_p = p + 12;
    copy_until_stop(obj_id, sizeof(obj_id), id_p, '|');

    /* 안전하게 아이디 파싱이 안 되면 그냥 무시 */
    if (obj_id[0] == '\0')
      return 1;

    if (s_laser_set)
      s_laser_set(1);

    if (s_tcp_send)
    {
      char resp[128];
      /* 서버 응답 형식은 예시이며(ACK 접두), 서버가 다른 포맷을 요구하면 여기서만 수정하면 됩니다. */
      snprintf(resp, sizeof(resp), "TRACK_START_ACK|%s\n", obj_id);
      s_tcp_send(resp);
    }
    return 1;
  }

  /* TRACK_POS|<object_id>|L=...|T=...|... */
  if (strncmp(p, "TRACK_POS|", 11) == 0)
  {
    /* 지금 단계에서는 ACK/서보 구동은 요청사항이 아니므로 "인식만" 처리 */
    return 1;
  }

  /* TRACK_END|<object_id>|REASON=<...> */
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
        snprintf(resp, sizeof(resp), "TRACK_END_ACK|%s|REASON=%s\n", obj_id[0] ? obj_id : "UNKNOWN", reason);
      else
        snprintf(resp, sizeof(resp), "TRACK_END_ACK|%s\n", obj_id[0] ? obj_id : "UNKNOWN");
      s_tcp_send(resp);
    }
    return 1;
  }

  return 0;
}

