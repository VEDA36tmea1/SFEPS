#include "servo_driver.h"
#include "main.h"

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;

static uint32_t clamp_us(uint32_t us)
{
  if (us < PWM_US_MIN) us = PWM_US_MIN;
  if (us > PWM_US_MAX) us = PWM_US_MAX;
  return us;
}

void Servo_Init(void)
{
  /* 기본 위치 1500us로 설정 후 PWM 시작 */
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 1500);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 1500);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  __HAL_TIM_MOE_ENABLE(&htim1);  /* TIM1(PA8) 실제 출력 위해 필수 */
}

void Servo_SetAllUs(uint32_t us_ch1, uint32_t us_ch2)
{
  us_ch1 = clamp_us(us_ch1);
  us_ch2 = clamp_us(us_ch2);

  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, us_ch1);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, us_ch2);
}

void Servo_SetCh1Us(uint32_t us)
{
  us = clamp_us(us);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, us);
}

void Servo_SetCh2Us(uint32_t us)
{
  us = clamp_us(us);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, us);
}

