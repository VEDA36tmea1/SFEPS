#pragma once

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LD2(보드 LED, PA5 D13) 초기화용 래퍼.
 *
 * 현재 GPIO 자체 초기화는 MX_GPIO_Init()에서 수행되므로,
 * 필요시 초기 상태만 정리하는 용도로 사용한다.
 */
void Led_Init(void);

/**
 * @brief 자동 모드 여부에 따라 LD2 상태를 바꾼다.
 *
 * @param auto_on 1이면 자동 모드 LED ON, 0이면 OFF.
 */
void Led_SetAutoMode(uint8_t auto_on);

#ifdef __cplusplus
}
#endif

