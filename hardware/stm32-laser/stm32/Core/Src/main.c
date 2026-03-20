/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "servo_driver.h"
#include "led_driver.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* RX 라인 버퍼 크기 (문자열 파싱용) */
#define RX_LINE_MAX   64

/* 제어 모드 */
#define MODE_MANUAL   0  /* UART로 들어온 us 값 그대로 사용 */
#define MODE_AUTO     1  /* 1200~1800 us 자동 스윕 */

/* 1200~1800 us 자동 회전(스윕) */
#define AUTO_PWM_MIN  1200
#define AUTO_PWM_MAX  1800
#define AUTO_STEP_US  10
#define AUTO_MS       30
#define AUTO_HOLD_MS  2000   /* UART 입력 후 이 시간(ms) 동안 고정, 이후 스윕 재개 (AUTO 모드에서만 사용) */

/* ESP8266 → 라즈베리 TCP 서버 자동 재접속 설정 */
#define WIFI_SERVER_IP           "192.168.4.1"
#define WIFI_SERVER_PORT         5565
#define WIFI_RECONNECT_INTERVAL  5000u   /* ms 단위: 5초마다 상태 체크 */
#define WIFI_CMD_TIMEOUT         10000u  /* AT 응답 타임아웃 10초 */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart2_rx;

/* USER CODE BEGIN PV */
#define WIFI_RX_DMA_SIZE  256
static uint8_t   wifi_rx_dma_buffer[WIFI_RX_DMA_SIZE];
volatile uint8_t wifi_rx_pending = 0;   /* it.c USART1_IRQHandler에서 설정 */
volatile uint16_t wifi_rx_len = 0;
static uint8_t   rx_byte;
static char      rx_line_buf[RX_LINE_MAX];
static uint8_t   rx_idx;
static volatile uint8_t rx_ready;
/* WiFi(TCP) 연결 상태 */
static uint8_t   wifi_link_ok      = 0;
static uint8_t   wifi_connecting   = 0;
static uint32_t  wifi_last_check   = 0;
static uint32_t  wifi_last_cmd_tick = 0;
/* WiFi(TCP)로 보낼 사용자 데이터(PING→PONG 등)를 AT+CIPSEND로 전송하기 위한 버퍼 */
static uint8_t   wifi_send_pending = 0;
static char      wifi_send_buf[WIFI_RX_DMA_SIZE];
static uint16_t  wifi_send_len = 0;

/* UART 디버그: 콜백이 불리는지 확인용 에코 플래그 */
static volatile uint8_t debug_rx_flag = 0;
static uint8_t          debug_rx_byte = 0;

/* B1 버튼으로 AUTO 모드 진입 요청 플래그 */
static volatile uint8_t button_auto_pending = 0;

/* 제어 모드: 기본은 수동(MANUAL) */
static uint8_t   control_mode = MODE_MANUAL;

/* 자동 스윕: 1200~1800 us 왕복 (AUTO 모드에서만 사용) */
static uint32_t  auto_pwm_val   = 1250;
static int32_t   auto_dir       = AUTO_STEP_US;
static uint32_t  auto_last_tick = 0;
static uint32_t  last_uart_tick = 0;  /* UART로 값 보낸 시각; 이 후 AUTO_HOLD_MS 동안 스윕 정지 */

/* WiFi 재접속 로그 스팸 방지를 위한 플래그 */
static uint8_t   wifi_reconnect_msg_shown = 0;

/* IBVS용 픽셀 오차 입력 (PC → STM32) 및 PID 상태 */
static float     ibvs_err_x      = 0.0f;
static float     ibvs_err_y      = 0.0f;
static uint8_t   ibvs_err_valid  = 0;
static uint32_t  ibvs_last_err_tick = 0;  /* 마지막으로 오차를 받은 시각(ms) */
/* 위치형 PID 기준 중립 PWM(us) – Servo_Init 이후 CCR에서 가져와 저장 */
static float     ibvs_neutral_x  = 1430.0f; /* PAN  (PA0 / TIM2_CH1) */
static float     ibvs_neutral_y  = 1250.0f; /* TILT (PA8 / TIM1_CH1) */

typedef struct
{
  float kp;
  float ki;
  float kd;
  float integ;
  float prev_err;
  float out_us;
} IbvsPidAxis;

static IbvsPidAxis pid_x; /* PAN  (PA0 / TIM2_CH1) */
static IbvsPidAxis pid_y; /* TILT (PA8 / TIM1_CH1) */

static uint32_t    ibvs_last_update_tick = 0;
#define IBVS_UPDATE_PERIOD_MS  15u   /* IBVS PID 업데이트 주기 ≈ 50Hz */
#define IBVS_ERR_TIMEOUT_MS  500u    /* 이 시간 동안 새 오차가 없으면 PID 정지 */

/* PID 로그 출력(USART2) 설정: 오프라인 튜닝용
 * - 1u : PIDLOG 라인 주기적으로 출력
 * - 0u : 로그 완전 비활성화
 */
#define IBVS_PID_LOG_ENABLE      0u
#define IBVS_PID_LOG_PERIOD_MS  100u
static uint32_t ibvs_pid_log_last_tick = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static void Wifi_SendLine(const char *line);
static void Wifi_MaybeReconnect(void);
static void IbvsPid_Init(void);
static void IbvsPid_Update(float dt_sec);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static float clampf(float v, float vmin, float vmax)
{
  if (v < vmin) return vmin;
  if (v > vmax) return vmax;
  return v;
}

/* IBVS PID 축 초기화 (PAN/TILT 공통) */
static void IbvsPid_AxisInit(IbvsPidAxis *a, float kp, float ki, float kd, float initial_us)
{
  if (!a) return;
  a->kp      = kp;
  a->ki      = ki;
  a->kd      = kd;
  a->integ   = 0.0f;
  a->prev_err= 0.0f;
  a->out_us  = initial_us;
}

static void IbvsPid_Init(void)
{
  /* STM32 Servo_Init 이후, 실제 타이머 비교 레지스터(CCR)에서
     현재 PWM(us) 값을 읽어와서 IBVS PID의 초기 출력값으로 사용한다.
     - PAN (X축)  : PA0 / TIM2_CH1
     - TILT (Y축) : PA8 / TIM1_CH1
     X축: 오른쪽으로 갈수록 PWM 증가 → Kp_x > 0
     Y축: 아래로 갈수록 PWM 감소   → Kp_y < 0
   */
  uint32_t ux_init = __HAL_TIM_GET_COMPARE(&htim2, TIM_CHANNEL_1); /* PA0 */
  uint32_t uy_init = __HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_1); /* PA8 */
  /* 예상 범위를 벗어나면 Servo_Init 기본값으로 폴백 */
  if (ux_init < PWM_US_MIN || ux_init > PWM_US_MAX) ux_init = 1430u;
  if (uy_init < PWM_US_MIN || uy_init > PWM_US_MAX) uy_init = 1250u;

  ibvs_neutral_x = (float)ux_init;
  ibvs_neutral_y = (float)uy_init;

  /* 위치형 PID – out_us 초기값을 중립으로 설정 */
  IbvsPid_AxisInit(&pid_x, 0.20f, 0.0f, 0.0f, ibvs_neutral_x);
  IbvsPid_AxisInit(&pid_y, -0.20f, 0.0f, 0.0f, ibvs_neutral_y);

  ibvs_err_x = 0.0f;
  ibvs_err_y = 0.0f;
  ibvs_err_valid = 0;
  ibvs_last_err_tick = 0;
  ibvs_last_update_tick = HAL_GetTick();
  ibvs_pid_log_last_tick = ibvs_last_update_tick;
}

/* IBVS PID 업데이트: 픽셀 오차(ibvs_err_x, ibvs_err_y)를 이용해 PWM을 갱신
   - 증분형이 아니라 "위치형 P" 제어로 구현해서 누적 오버슈트를 줄인다. */
static void IbvsPid_Update(float dt_sec)
{
  (void)dt_sec; /* 현재 위치형 P 제어에서는 dt_sec 미사용 */

  uint32_t now = HAL_GetTick();
  float ex = ibvs_err_x;
  float ey = ibvs_err_y;

  /* 타임아웃 또는 유효하지 않은 오차인 경우 → 중립 위치로 복귀 */
  if (!ibvs_err_valid || (now - ibvs_last_err_tick > IBVS_ERR_TIMEOUT_MS))
  {
    pid_x.out_us = ibvs_neutral_x;
    pid_y.out_us = ibvs_neutral_y;
  }
  else
  {
    /* 위치형 P 제어: 매번 "중립 + Kp * error" 로 절대 위치 계산 (누적 없음) */
    pid_x.out_us = ibvs_neutral_x + pid_x.kp * ex; /* X(PAN, PA0) – 오른쪽으로 갈수록 PWM 증가 */
    pid_y.out_us = ibvs_neutral_y + pid_y.kp * ey; /* Y(TILT, PA8) – 아래로 갈수록 PWM 감소(Kp<0) */

    /* 작은 오차(데드존)는 무시해서 떨림 감소 */
    if (fabsf(ex) < 5.0f)
      pid_x.out_us = ibvs_neutral_x;
    if (fabsf(ey) < 5.0f)
      pid_y.out_us = ibvs_neutral_y;
  }

  /* 범위 제한 */
  pid_x.out_us = clampf(pid_x.out_us, (float)PWM_US_MIN, (float)PWM_US_MAX);
  pid_y.out_us = clampf(pid_y.out_us, (float)PWM_US_MIN, (float)PWM_US_MAX);

  /* 실제 서보 PWM 업데이트: PA0=X(PAN), PA8=Y(TILT) */
  uint32_t ux = (uint32_t)pid_x.out_us; /* CH2 */
  uint32_t uy = (uint32_t)pid_y.out_us; /* CH1 */
  Servo_SetCh2Us(ux);
  Servo_SetCh1Us(uy);
#if IBVS_PID_LOG_ENABLE
  {
    uint32_t now_log = HAL_GetTick();
    if ((now_log - ibvs_pid_log_last_tick) >= IBVS_PID_LOG_PERIOD_MS)
    {
      ibvs_pid_log_last_tick = now_log;
      char log_buf[160];
      int n = snprintf(log_buf, sizeof(log_buf),
                       "PIDLOG,t:%lu,ex:%.2f,ey:%.2f,out_x:%lu,out_y:%lu,kpx:%.4f,kix:%.4f,kdx:%.4f,kpy:%.4f,kiy:%.4f,kdy:%.4f\r\n",
                       (unsigned long)now_log,
                       (double)ex,
                       (double)ey,
                       (unsigned long)ux,
                       (unsigned long)uy,
                       (double)pid_x.kp,
                       (double)pid_x.ki,
                       (double)pid_x.kd,
                       (double)pid_y.kp,
                       (double)pid_y.ki,
                       (double)pid_y.kd);
      if (n > 0)
      {
        HAL_UART_Transmit(&huart2, (uint8_t *)log_buf, (uint16_t)n, 30);
      }
    }
  }
#endif
}
/* ESP8266으로 AT 명령 한 줄(문자열 + CRLF) 전송 */
static void Wifi_SendLine(const char *line)
{
  size_t len = strlen(line);
  if (len > 0)
  {
    HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)len, 100);
  }
  const char crlf[2] = {'\r', '\n'};
  HAL_UART_Transmit(&huart1, (const uint8_t *)crlf, 2, 100);
}

/* 주기적으로 TCP 서버(라즈베리) 재접속 시도 */
static void Wifi_MaybeReconnect(void)
{
  uint32_t now = HAL_GetTick();

  /* 연결 시도 중인데 응답이 너무 오래 없으면 실패로 간주 */
  if (wifi_connecting && (now - wifi_last_cmd_tick) > WIFI_CMD_TIMEOUT)
  {
    wifi_connecting = 0;
    wifi_link_ok = 0;
  }

  /* 이미 연결된 상태면 아무 것도 안 함 */
  if (wifi_link_ok || wifi_connecting)
    return;

  /* 일정 주기마다만 재접속 시도 */
  if ((now - wifi_last_check) < WIFI_RECONNECT_INTERVAL)
    return;
  wifi_last_check = now;

  char cmd[80];
  int n = snprintf(cmd, sizeof(cmd),
                   "AT+CIPSTART=\"TCP\",\"%s\",%d",
                   WIFI_SERVER_IP, WIFI_SERVER_PORT);
  if (n > 0)
  {
    Wifi_SendLine(cmd);
    wifi_connecting    = 1;
    wifi_last_cmd_tick = now;

    /* 아직 한 번도 안내를 찍지 않았다면, 재접속 시도 안내를 한 줄만 출력 */
    if (!wifi_reconnect_msg_shown)
    {
      const char *info = "WiFi: try reconnect TCP (시도중)\r\n";
      HAL_UART_Transmit(&huart2, (const uint8_t *)info, (uint16_t)strlen(info), 50);
      wifi_reconnect_msg_shown = 1;
    }
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 부팅 직후 USART2(PA2/PA3, ST-LINK VCP)로만 출력 → 터미널에서 보이면 포트/보드레이트 OK */
  HAL_Delay(200);
  {
    const char boot[] = "\r\n[STM32 boot]\r\n";
    (void)HAL_UART_Transmit(&huart2, (const uint8_t *)boot, (uint16_t)(sizeof(boot) - 1), 100);
  }
  rx_idx = 0;
  rx_ready = 0;
  /* WiFi(USART1) DMA 수신 시작 – IDLE 시 한 줄씩 처리 */
  (void)HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
  /* PWM 주기: PWM_FREQ_HZ에 따라 ARR 설정 (1 tick = 1 us) */
  {
    uint32_t pwm_period = (1000000u / (uint32_t)PWM_FREQ_HZ) - 1u;
    __HAL_TIM_SET_AUTORELOAD(&htim1, pwm_period);
    __HAL_TIM_SET_AUTORELOAD(&htim2, pwm_period);
  }
  Servo_Init();
  HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
  auto_last_tick = HAL_GetTick();
  Led_Init();
  Led_SetAutoMode(control_mode == MODE_AUTO);
  /* IBVS PID 초기화 (호스트에서 픽셀 오차를 보내는 경우에 사용) */
  IbvsPid_Init();
  {
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
                     "%dHz PWM. MODE 0=manual, 1=auto.\r\n"
                     "WiFi(USART1) or UART2: 1500 or 1500 1200 or \"mode 0/1\".\r\n",
                     (int)PWM_FREQ_HZ);
    if (n > 0)
    {
      HAL_UART_Transmit(&huart2, (uint8_t *)msg, (uint16_t)n, 100);
    }
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* B1 버튼으로 모드 토글 (MANUAL ↔ AUTO) */
    if (button_auto_pending)
    {
      button_auto_pending = 0;
      if (control_mode == MODE_AUTO)
      {
        /* AUTO → MANUAL로 전환: 현재 각도 유지, 스윕 중단 */
        control_mode = MODE_MANUAL;
        Led_SetAutoMode(control_mode == MODE_AUTO);
        const char *msg = "manual mode 실행\r\n";
        HAL_UART_Transmit(&huart2, (const uint8_t *)msg, (uint16_t)strlen(msg), 50);
      }
      else
      {
        /* MANUAL → AUTO로 전환: 1200~1800 스윕 시작 */
        control_mode   = MODE_AUTO;
        auto_pwm_val   = AUTO_PWM_MIN;
        auto_dir       = AUTO_STEP_US;
        auto_last_tick = HAL_GetTick();
        last_uart_tick = 0;
        Led_SetAutoMode(control_mode == MODE_AUTO);
        const char *msg = "auto mode 실행\r\n";
        HAL_UART_Transmit(&huart2, (const uint8_t *)msg, (uint16_t)strlen(msg), 50);
      }
    }

    /* 디버그: 수신된 마지막 바이트를 에코 (USART2용) */
    if (debug_rx_flag)
    {
      debug_rx_flag = 0;
      HAL_UART_Transmit(&huart2, &debug_rx_byte, 1, 20);
    }

    /* WiFi(USART1) DMA IDLE로 한 줄 수신 완료 시: ESP → PC 시리얼로 그대로 에코 + 상태 파싱 */
    if (wifi_rx_pending)
    {
      wifi_rx_pending = 0;
      uint16_t len = wifi_rx_len;
      if (len > 0)
      {
        /* 문자열로 처리하기 위해 로컬 버퍼에 복사 & 널 종료 */
        char wifi_line[WIFI_RX_DMA_SIZE];
        if (len >= WIFI_RX_DMA_SIZE)
          len = WIFI_RX_DMA_SIZE - 1;
        for (uint16_t i = 0; i < len; i++)
          wifi_line[i] = (char)wifi_rx_dma_buffer[i];
        wifi_line[len] = '\0';
        const char crlf[2] = {'\r', '\n'};

        /* ESP → PC: 연결이 성립된 이후에만 에코.
           - 자동 재접속(AT+CIPSTART / ERROR / CLOSED) 반복 시 터미널 스팸을 줄이기 위해
           - wifi_link_ok == 1 인 상태에서만 WiFi 모듈의 원문 라인을 보여준다. */
        if (wifi_link_ok)
        {
          HAL_UART_Transmit(&huart2, (uint8_t *)wifi_line, len, 50);
          HAL_UART_Transmit(&huart2, (uint8_t*)crlf, 2, 50);
        }

        /* ------------------------------------------------------------------ */
        /* 1) 좌표/오차/펄스 명령 처리 (+IPD, ... EX=...,EY=... / CX=...,CY=... / "1500 1400" 등) */
        /* ------------------------------------------------------------------ */
        {
          char *ipd = strstr(wifi_line, "+IPD");
          if (ipd != NULL)
          {
            char *payload = strchr(ipd, ':');
            if (payload != NULL)
            {
              payload++; /* ':' 뒤부터 실제 데이터 */
              /* 끝 CR/LF 제거 */
              char *pend = payload + strlen(payload);
              while (pend > payload &&
                     (pend[-1] == '\r' || pend[-1] == '\n'))
              {
                *--pend = '\0';
              }

              /* payload 안에 여러 줄(EX=...,EY=... / CX=...,CY=... / "1500 1400" 등)이
                 한꺼번에 들어올 수 있으므로, 줄 단위로 쪼개서 각각 처리한다. */
              char *cursor = payload;
              while (*cursor)
              {
                /* 앞쪽 개행/공백 스킵 */
                while (*cursor == '\r' || *cursor == '\n')
                  cursor++;
                if (!*cursor)
                  break;

                /* 한 줄 끝 찾기 */
                char *line_end = cursor;
                while (*line_end && *line_end != '\r' && *line_end != '\n')
                  line_end++;

                char saved = *line_end;
                *line_end = '\0';

                /* 형식 0: EX=...,EY=... (ubuntu_tcp_server에서 보낸 픽셀 오차 등)
                 * - EX : X축 오차 (픽셀)
                 * - EY : Y축 오차 (픽셀)
                 *   이 값들은 STM32 내부 IBVS PID 제어기의 입력으로 사용된다.
                 */
                float ex_f = 0.0f, ey_f = 0.0f;
                float cx_f = 0.0f, cy_f = 0.0f;
                if (sscanf(cursor, "EX=%f,EY=%f", &ex_f, &ey_f) == 2)
                {
                  ibvs_err_x        = ex_f;
                  ibvs_err_y        = ey_f;
                  ibvs_err_valid    = 1;
                  ibvs_last_err_tick = HAL_GetTick();
                  /* 디버그: WiFi로 들어온 오차 값을 USART2로 표시 (필요시 활성화) */
                  /*
                  char dbg[80];
                  int dn = snprintf(dbg, sizeof(dbg),
                                    "WiFi ERR EX/EY -> ex=%.2f ey=%.2f\r\n",
                                    (double)ex_f, (double)ey_f);
                  if (dn > 0)
                  {
                    HAL_UART_Transmit(&huart2, (uint8_t *)dbg, (uint16_t)dn, 50);
                  }
                  */
                }
                else if (sscanf(cursor, "CX=%f,CY=%f", &cx_f, &cy_f) == 2)
                {
                  /* 형식 1: CX=...,CY=... (이전 방식 – 직접 PWM 명령)
                   * - CX : X축 (PA0 / TIM2_CH1)
                   * - CY : Y축 (PA8 / TIM1_CH1)
                   */
                  uint32_t u1 = (uint32_t)cx_f;
                  uint32_t u2 = (uint32_t)cy_f;
                  if (u1 < PWM_US_MIN) u1 = PWM_US_MIN;
                  if (u1 > PWM_US_MAX) u1 = PWM_US_MAX;
                  if (u2 < PWM_US_MIN) u2 = PWM_US_MIN;
                  if (u2 > PWM_US_MAX) u2 = PWM_US_MAX;
                  /* CY -> PA8(TIM1_CH1), CX -> PA0(TIM2_CH1) */
                  Servo_SetAllUs(u2, u1);
                  auto_pwm_val   = u2;
                  last_uart_tick = HAL_GetTick();
                  /* 디버그: WiFi로 들어온 PWM 값을 USART2로 표시 */
                  char dbg[80];
                  int dn = snprintf(dbg, sizeof(dbg),
                                    "WiFi PWM CX/CY -> PA8(CY)=%lu PA0(CX)=%lu us\r\n",
                                    (unsigned long)u2, (unsigned long)u1);
                  if (dn > 0)
                  {
                    HAL_UART_Transmit(&huart2, (uint8_t *)dbg, (uint16_t)dn, 50);
                  }
                }
                else
                {
                  /* 형식 2: "1500 1400" 또는 "1500"
                   * - 첫 번째 값: X축(PA0 / TIM2_CH1)
                   * - 두 번째 값: Y축(PA8 / TIM1_CH1)
                   */
                  unsigned long ux = 1500, uy = 1500;
                  int n_ipd = sscanf(cursor, "%lu %lu", &ux, &uy);
                  if (n_ipd >= 1)
                  {
                    if (ux < PWM_US_MIN) ux = PWM_US_MIN;
                    if (ux > PWM_US_MAX) ux = PWM_US_MAX;
                    if (n_ipd >= 2)
                    {
                      if (uy < PWM_US_MIN) uy = PWM_US_MIN;
                      if (uy > PWM_US_MAX) uy = PWM_US_MAX;
                    }
                    else
                    {
                      uy = ux;
                    }
                    /* uy -> PA8(TIM1_CH1), ux -> PA0(TIM2_CH1) */
                    Servo_SetAllUs((uint32_t)uy, (uint32_t)ux);
                    auto_pwm_val   = (uint32_t)uy;
                    last_uart_tick = HAL_GetTick();
                    /* 디버그: WiFi로 들어온 정수 PWM 값을 USART2로 표시 */
                    char dbg[80];
                    int dn = snprintf(dbg, sizeof(dbg),
                                      "WiFi PWM INT -> PA8(Y)=%lu PA0(X)=%lu us\r\n",
                                      (unsigned long)uy, (unsigned long)ux);
                    if (dn > 0)
                    {
                      HAL_UART_Transmit(&huart2, (uint8_t *)dbg, (uint16_t)dn, 50);
                    }
                  }
                }

                /* 원래 문자 복원 후 다음 줄로 진행 */
                *line_end = saved;
                if (!saved)
                  break;
                cursor = line_end + 1;
              }
            }
          }
        }

        /* 애플리케이션 레벨 PING/PONG 처리: Pi → ESP → STM → ESP → Pi 왕복 측정 */
        /* ESP AT 모드에서는 "+IPD,xx:PING,..." 형태로 들어올 수 있으므로,
           라인 어디에서든 "PING," 서브스트링을 찾아서 처리한다. */
        char *ping_pos = strstr(wifi_line, "PING,");
        if (ping_pos != NULL && !wifi_send_pending)
        {
          const char *payload = ping_pos + 5;          /* "PING," 뒤 payload (seq,t0_ms...) */

          /* payload 끝에서 개행/캐리지리턴 제거 */
          size_t payload_len = strlen(payload);
          while (payload_len &&
                 (payload[payload_len - 1] == '\r' ||
                  payload[payload_len - 1] == '\n'))
          {
            payload_len--;
          }

          /* 1) 보낼 데이터(PONG,<payload>\n)를 전역 버퍼에 준비 */
          if (5u + payload_len + 1u < sizeof(wifi_send_buf))
          {
            size_t idx = 0;
            memcpy(&wifi_send_buf[idx], "PONG,", 5);
            idx += 5;
            memcpy(&wifi_send_buf[idx], payload, payload_len);
            idx += payload_len;
            wifi_send_buf[idx++] = '\n';      /* RTT 측정을 위한 개행 */
            wifi_send_buf[idx]   = '\0';      /* 디버그 출력용 */
            wifi_send_len        = (uint16_t)idx;

            /* 2) AT+CIPSEND=<len> 을 먼저 전송 (ESP가 '>' 프롬프트를 보냄) */
            char cmd[48];
            int cn = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u",
                              (unsigned)wifi_send_len);
            if (cn > 0)
            {
              Wifi_SendLine(cmd);             /* CRLF 포함 AT+CIPSEND=... */
              wifi_last_cmd_tick = HAL_GetTick();
              wifi_send_pending  = 1;

              /* PC 터미널에도 디버그 출력 */
              HAL_UART_Transmit(&huart2,
                                (uint8_t *)cmd,
                                (uint16_t)cn,
                                50);
              HAL_UART_Transmit(&huart2,
                                (const uint8_t *)crlf,
                                2,
                                50);
            }
          }
        }
        else
        {
          /* WiFi/TCP 상태 업데이트 + CIPSEND 프롬프트 처리 */
          /* 1) CIPSEND '>' 프롬프트 감지 후, 준비된 데이터(wifi_send_buf)를 실제 TCP 페이로드로 송신 */
          if (wifi_send_pending && strchr(wifi_line, '>') != NULL)
          {
            HAL_UART_Transmit(&huart1,
                              (uint8_t *)wifi_send_buf,
                              wifi_send_len,
                              200);
            wifi_send_pending = 0;

            /* PC 터미널에도 송신 내용을 표시 */
            HAL_UART_Transmit(&huart2,
                              (uint8_t *)wifi_send_buf,
                              wifi_send_len,
                              50);
            HAL_UART_Transmit(&huart2,
                              (const uint8_t *)crlf,
                              2,
                              50);
          }
          /* 2) 명확한 끊김 패턴 - CLOSED / STATUS:4 는 항상 연결 끊김으로 간주 */
          else if (strstr(wifi_line, "CLOSED") != NULL ||
                   strstr(wifi_line, "STATUS:4") != NULL)
          {
            wifi_link_ok     = 0;
            wifi_connecting  = 0;
            wifi_send_pending = 0;
          }
          /* 3) 명확한 성공/연결 패턴들을 먼저 처리 (한 줄에 OK + ERROR 같이 있어도 "성공 우선") */
          else if (strstr(wifi_line, "ALREADY CONNECTED") != NULL ||
                   strstr(wifi_line, "CONNECT") != NULL ||
                   strstr(wifi_line, "STATUS:3") != NULL ||
                   strstr(wifi_line, "+IPD") != NULL ||
                   strstr(wifi_line, "SEND OK") != NULL ||
                   strstr(wifi_line, "OK") != NULL)
          {
            wifi_link_ok    = 1;
            wifi_connecting = 0;
            /* 연결이 성립되면 재접속 안내 문구를 다시 찍을 수 있도록 리셋 */
            wifi_reconnect_msg_shown = 0;
          }
          /* 4) ERROR / FAIL
           *  - 아직 연결 안 된 상태(wifi_link_ok == 0)에서 나오면 "연결 실패"로만 처리
           *  - 이미 연결 OK 상태라면(예: ALREADY CONNECTED 이후) 단순 재요청 실패로 보고 연결은 유지  */
          else if (strstr(wifi_line, "ERROR") != NULL ||
                   strstr(wifi_line, "FAIL")  != NULL)
          {
            /* 연결 OK 상태면 wifi_link_ok 유지, 진행 중이던 연결 시도/송신만 중단 */
            wifi_connecting   = 0;
            wifi_send_pending = 0;
          }
        }
      }
      (void)HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);
    }

    if (rx_ready)
    {
      rx_ready = 0;
      rx_line_buf[rx_idx] = '\0';

      /* 공백/개행만 들어온 경우 무시 */
      if (rx_idx == 0 || rx_line_buf[0] == '\r' || rx_line_buf[0] == '\n')
      {
        rx_idx = 0;
        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
        continue;
      }

      /* ESP: 프리픽스로 시작하는 라인은 ESP(USART1)로 그대로 AT 명령 전달 */
      if (strncmp(rx_line_buf, "ESP:", 4) == 0 || strncmp(rx_line_buf, "esp:", 4) == 0)
      {
        char *cmd = rx_line_buf + 4;
        /* 프리픽스 뒤 공백 스킵 */
        while (*cmd == ' ' || *cmd == '\t')
          cmd++;
        if (*cmd != '\0')
        {
          Wifi_SendLine(cmd);  /* ESP(USART1)로 전송 (CRLF 자동 첨부) */

          /* PC 터미널에도 내가 보낸 ESP 명령을 에코 */
          size_t cmd_len = strlen(cmd);
          HAL_UART_Transmit(&huart2, (uint8_t *)"ESP:", 4, 50);
          HAL_UART_Transmit(&huart2, (uint8_t *)cmd, (uint16_t)cmd_len, 50);
          const char crlf2[2] = {'\r', '\n'};
          HAL_UART_Transmit(&huart2, (const uint8_t *)crlf2, 2, 50);
        }
        rx_idx = 0;
        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
        continue;
      }

      /* AT 명령: PC(USART2)에서 들어온 라인을 ESP(USART1)로 그대로 패스스루 */
      if (strncmp(rx_line_buf, "AT", 2) == 0 || strncmp(rx_line_buf, "at", 2) == 0)
      {
        size_t at_len = strlen(rx_line_buf);
        /* ESP 쪽으로 AT 라인 + CRLF 전송 */
        if (at_len > 0)
        {
          Wifi_SendLine(rx_line_buf);

          /* PC 터미널에도 내가 보낸 AT 명령을 에코 */
          HAL_UART_Transmit(&huart2, (uint8_t *)rx_line_buf, (uint16_t)at_len, 100);
          const char crlf2[2] = {'\r', '\n'};
          HAL_UART_Transmit(&huart2, (const uint8_t *)crlf2, 2, 100);
        }
        rx_idx = 0;
        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
        continue;
      }

      /* MODE 명령 처리: "mode 0" 또는 "mode 1" */
      if (strncmp(rx_line_buf, "mode", 4) == 0)
      {
        unsigned long m = 0;
        if (sscanf(rx_line_buf + 4, "%lu", &m) == 1 && (m == 0ul || m == 1ul))
        {
          control_mode = (uint8_t)m;
          Led_SetAutoMode(control_mode == MODE_AUTO);
          const char *resp = (control_mode == MODE_MANUAL)
                             ? "MODE=0 (manual)\r\n"
                             : "MODE=1 (auto sweep 1200~1800us)\r\n";
          HAL_UART_Transmit(&huart2, (const uint8_t *)resp, (uint16_t)strlen(resp), 50);
        }
        else
        {
          const char *err = "Usage: mode 0 (manual) or mode 1 (auto)\r\n";
          HAL_UART_Transmit(&huart2, (const uint8_t *)err, (uint16_t)strlen(err), 50);
        }
      }
      else
      {
        /* 기본: 서보 제어 명령
         * - "1500" 또는 "1500 1200"  → 두 채널 모두/각각 설정
         * - "X:1500" 또는 "X 1500"   → PA0(TIM2_CH1)만 설정
         * - "Y:1500" 또는 "Y 1500"   → PA8(TIM1_CH1)만 설정
         */
        char *p = rx_line_buf;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == 'X' || *p == 'x')
        {
          p++; /* 'X' 지나침 */
          if (*p == ':' || *p == ' ') p++;
          unsigned long ux = 1500;
          if (sscanf(p, "%lu", &ux) == 1)
          {
            if (ux < PWM_US_MIN) ux = PWM_US_MIN;
            if (ux > PWM_US_MAX) ux = PWM_US_MAX;
            /* X: PA0(TIM2_CH1)만 변경 */
            Servo_SetCh2Us((uint32_t)ux);
            last_uart_tick = HAL_GetTick();
            char ack[64];
            int len = snprintf(ack, sizeof(ack), "\nOK X=PA0=%lu us\r\n", ux);
            if (len > 0)
            {
              HAL_UART_Transmit(&huart2, (uint8_t *)ack, (uint16_t)len, 50);
            }
          }
          else
          {
            const char *err = "? Usage: X:1500\r\n";
            HAL_UART_Transmit(&huart2, (const uint8_t *)err, (uint16_t)strlen(err), 50);
          }
        }
        else if (*p == 'Y' || *p == 'y')
        {
          p++; /* 'Y' 지나침 */
          if (*p == ':' || *p == ' ') p++;
          unsigned long uy = 1500;
          if (sscanf(p, "%lu", &uy) == 1)
          {
            if (uy < PWM_US_MIN) uy = PWM_US_MIN;
            if (uy > PWM_US_MAX) uy = PWM_US_MAX;
            /* Y: PA8(TIM1_CH1)만 변경 */
            Servo_SetCh1Us((uint32_t)uy);
            auto_pwm_val = (uint32_t)uy;
            last_uart_tick = HAL_GetTick();
            char ack[64];
            int len = snprintf(ack, sizeof(ack), "\nOK Y=PA8=%lu us\r\n", uy);
            if (len > 0)
            {
              HAL_UART_Transmit(&huart2, (uint8_t *)ack, (uint16_t)len, 50);
            }
          }
          else
          {
            const char *err = "? Usage: Y:1500\r\n";
            HAL_UART_Transmit(&huart2, (const uint8_t *)err, (uint16_t)strlen(err), 50);
          }
        }
        else
        {
          /* 기본: "1500" 또는 "1500 1200" 형식으로 us 값 설정 */
          unsigned long u1 = 1500, u2 = 1500;
          int n = sscanf(rx_line_buf, "%lu %lu", &u1, &u2);
          if (n >= 1)
          {
            if (u1 < PWM_US_MIN) u1 = PWM_US_MIN;
            if (u1 > PWM_US_MAX) u1 = PWM_US_MAX;
            if (n >= 2)
            {
              if (u2 < PWM_US_MIN) u2 = PWM_US_MIN;
              if (u2 > PWM_US_MAX) u2 = PWM_US_MAX;
            }
            else
            {
              u2 = u1;
            }
            /* CH1=PA8(TIM1), CH2=PA0(TIM2) */
            Servo_SetAllUs((uint32_t)u1, (uint32_t)u2);
            auto_pwm_val = (uint32_t)u1;
            last_uart_tick = HAL_GetTick();
            char ack[52];
            int len = snprintf(ack, sizeof(ack), "\nOK PA8=%lu PA0=%lu us\r\n", u1, u2);
            if (len > 0)
            {
              HAL_UART_Transmit(&huart2, (uint8_t *)ack, (uint16_t)len, 50);
            }
          }
          else
          {
            const char *err = "? (send: 1500 or 1500 1200, X:1500, Y:1500, or mode 0/1)\r\n";
            HAL_UART_Transmit(&huart2, (const uint8_t *)err, (uint16_t)strlen(err), 50);
          }
        }
      }
      rx_idx = 0;
      HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
    }

    /* IBVS PID 제어: 픽셀 오차(EX/EY)가 주기적으로 들어올 때 동작 */
    {
      uint32_t now = HAL_GetTick();

      /* 일정 시간 동안 새 오차가 없으면 PID 정지 (failsafe) */
      if (ibvs_err_valid && (now - ibvs_last_err_tick) > IBVS_ERR_TIMEOUT_MS)
      {
        ibvs_err_valid = 0;
      }

      /* 주기적으로 PID 업데이트 */
      uint32_t dt_ms = now - ibvs_last_update_tick;
      if (dt_ms >= IBVS_UPDATE_PERIOD_MS)
      {
        float dt_sec = (float)dt_ms / 1000.0f;
        ibvs_last_update_tick = now;
        IbvsPid_Update(dt_sec);
      }
    }

    /* 자동 스윕: MODE_AUTO에서만 동작, 기본은 비활성화(MODE_MANUAL) */
    if (control_mode == MODE_AUTO)
    {
      uint32_t now = HAL_GetTick();
      if (last_uart_tick != 0 && (now - last_uart_tick) < AUTO_HOLD_MS)
      {
        /* UART로 넣은 값 유지, 스윕 안 함 */
      }
      else if (now - auto_last_tick >= AUTO_MS)
      {
        if (last_uart_tick != 0 && (now - last_uart_tick) >= AUTO_HOLD_MS)
          last_uart_tick = 0;  /* 2초 지남 → 스윕 재개 */
        auto_last_tick = now;
        auto_pwm_val += (uint32_t)auto_dir;
        if (auto_pwm_val >= AUTO_PWM_MAX)
        {
          auto_pwm_val = AUTO_PWM_MAX;
          auto_dir = -(int32_t)AUTO_STEP_US;
        }
        else if (auto_pwm_val <= AUTO_PWM_MIN)
        {
          auto_pwm_val = AUTO_PWM_MIN;
          auto_dir = (int32_t)AUTO_STEP_US;
        }
        Servo_SetAllUs(auto_pwm_val, auto_pwm_val);
      }
    }

    /* 주기적으로 ESP8266 → 라즈베리 TCP 서버 재접속 시도 */
    Wifi_MaybeReconnect();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 83;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 19999;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;  /* 직결/버퍼 시 정극성; NPN 반전 회로면 LOW 사용 */
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_LOW;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 83;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;  /* 직결/버퍼 시 정극성; NPN 반전 회로면 LOW 사용 */
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_LOW;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* B1 사용자 버튼(PC13) EXTI15_10 인터럽트 활성화 */
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &huart2) return;
  /* 인터럽트 안에서는 전송 금지 → 타이밍/전류 변동으로 소리·간섭 발생 가능 */

  /* 디버그용: 어떤 바이트가 들어오는지 main 루프에서 에코할 수 있게 저장 */
  debug_rx_byte = rx_byte;
  debug_rx_flag = 1;

  if (rx_byte == '\r' || rx_byte == '\n')
  {
    rx_ready = 1;
  }
  else if (rx_idx < RX_LINE_MAX - 1)
  {
    rx_line_buf[rx_idx++] = (char)rx_byte;
  }
  else
  {
    rx_ready = 1; /* line full */
  }
  if (!rx_ready)
    HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == B1_Pin)
  {
    button_auto_pending = 1;
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
