#pragma once

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 서보 공통 파라미터 (us 단위 범위 등) */
#define PWM_FREQ_HZ   50    /* 50Hz, 20ms 주기 */
#define PWM_US_MIN    800
#define PWM_US_MAX    2200

/**
 * @brief 서보 PWM 초기화 래퍼.
 *
 * - TIM1 / TIM2 CH1 에 1500us 기본 펄스를 설정하고
 * - PWM 출력을 시작한다.
 *
 * 주의: 타이머 자체 초기화(MX_TIM1_Init / MX_TIM2_Init)는
 *       이 함수 호출 전에 이루어져 있어야 한다.
 */
void Servo_Init(void);

/**
 * @brief 두 채널(PA8=TIM1_CH1, PA0=TIM2_CH1)에 펄스 폭(us) 설정.
 *
 * @param us_ch1 TIM1_CH1 펄스 폭 (마이크로초)
 * @param us_ch2 TIM2_CH1 펄스 폭 (마이크로초)
 */
void Servo_SetAllUs(uint32_t us_ch1, uint32_t us_ch2);

/**
 * @brief CH1(TIM1_CH1)만 설정.
 */
void Servo_SetCh1Us(uint32_t us);

/**
 * @brief CH2(TIM2_CH1)만 설정.
 */
void Servo_SetCh2Us(uint32_t us);

#ifdef __cplusplus
}
#endif

