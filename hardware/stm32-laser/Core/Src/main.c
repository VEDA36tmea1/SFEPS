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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* PWM 주파수 (Hz): 50=서보 표준 20ms, 100/250 등 테스트 가능 */
#define PWM_FREQ_HZ   50
#define PWM_US_MIN    800
#define PWM_US_MAX    2200
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
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart2_rx;

/* USER CODE BEGIN PV */
static uint8_t   rx_byte;
static char      rx_line_buf[RX_LINE_MAX];
static uint8_t   rx_idx;
static volatile uint8_t rx_ready;

/* UART 디버그: 콜백이 불리는지 확인용 에코 플래그 */
static volatile uint8_t debug_rx_flag = 0;
static uint8_t          debug_rx_byte = 0;

/* B1 버튼으로 AUTO 모드 진입 요청 플래그 */
static volatile uint8_t button_auto_pending = 0;

/* 제어 모드: 기본은 수동(MANUAL) */
static uint8_t   control_mode = MODE_MANUAL;

/* 자동 스윕: 1200~1800 us 왕복 (AUTO 모드에서만 사용) */
static uint32_t  auto_pwm_val   = 1500;
static int32_t   auto_dir       = AUTO_STEP_US;
static uint32_t  auto_last_tick = 0;
static uint32_t  last_uart_tick = 0;  /* UART로 값 보낸 시각; 이 후 AUTO_HOLD_MS 동안 스윕 정지 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void PA5_D13_SetByMode(uint8_t mode)
{
  /* NUCLEO 계열에서 PA5는 보통 LD2(D13)로 연결됨 */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                    (mode == MODE_MANUAL) ? GPIO_PIN_SET : GPIO_PIN_RESET);
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
  /* USER CODE BEGIN 2 */
  rx_idx = 0;
  rx_ready = 0;
  /* PWM 주기: PWM_FREQ_HZ에 따라 ARR 설정 (1 tick = 1 us) */
  {
    uint32_t pwm_period = (1000000u / (uint32_t)PWM_FREQ_HZ) - 1u;
    __HAL_TIM_SET_AUTORELOAD(&htim1, pwm_period);
    __HAL_TIM_SET_AUTORELOAD(&htim2, pwm_period);
  }
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 1500);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 1500);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  __HAL_TIM_MOE_ENABLE(&htim1);  /* TIM1(PA8) 실제 출력 위해 필수 */
  HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
  auto_last_tick = HAL_GetTick();
  PA5_D13_SetByMode(control_mode);
  {
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
                     "%dHz PWM. MODE 0=manual, 1=auto.\r\n"
                     "Send: 1500 or 1500 1200 or \"mode 0/1\".\r\n",
                     (int)PWM_FREQ_HZ);
    if (n > 0) HAL_UART_Transmit(&huart2, (uint8_t *)msg, (uint16_t)n, 100);
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
        PA5_D13_SetByMode(control_mode);
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
        PA5_D13_SetByMode(control_mode);
        const char *msg = "auto mode 실행\r\n";
        HAL_UART_Transmit(&huart2, (const uint8_t *)msg, (uint16_t)strlen(msg), 50);
      }
    }

    /* 디버그: 수신된 마지막 바이트를 에코 (콜백이 실제로 불리는지 확인) */
    if (debug_rx_flag)
    {
      debug_rx_flag = 0;
      HAL_UART_Transmit(&huart2, &debug_rx_byte, 1, 20);
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

      /* MODE 명령 처리: "mode 0" 또는 "mode 1" */
      if (strncmp(rx_line_buf, "mode", 4) == 0)
      {
        unsigned long m = 0;
        if (sscanf(rx_line_buf + 4, "%lu", &m) == 1 && (m == 0ul || m == 1ul))
        {
          control_mode = (uint8_t)m;
          PA5_D13_SetByMode(control_mode);
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
          __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)u1);
          __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, (uint32_t)u2);
          auto_pwm_val = (uint32_t)u1;
          last_uart_tick = HAL_GetTick();
          char ack[52];
          int len = snprintf(ack, sizeof(ack), "\nOK PA8=%lu PA0=%lu us\r\n", u1, u2);
          if (len > 0)
            HAL_UART_Transmit(&huart2, (uint8_t *)ack, (uint16_t)len, 50);
        }
        else
        {
          const char *err = "? (send: 1500 or 1500 1200, or mode 0/1)\r\n";
          HAL_UART_Transmit(&huart2, (const uint8_t *)err, (uint16_t)strlen(err), 50);
        }
      }
      rx_idx = 0;
      HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
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
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, auto_pwm_val);
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, auto_pwm_val);
      }
    }
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
  htim1.Init.Period = 19999;  /* 50Hz, 20ms (20000us), 1us per tick */
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
  htim2.Init.Period = 19999;  /* 50Hz, 20ms (20000us), 1us per tick */
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
