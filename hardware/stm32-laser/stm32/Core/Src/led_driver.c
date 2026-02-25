#include "led_driver.h"
#include "main.h"

void Led_Init(void)
{
  /* MX_GPIO_Init()에서 LD2 핀은 이미 출력으로 설정됨.
   * 여기서는 초기 상태만 명시적으로 LOW로 두어도 된다. */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
}

void Led_SetAutoMode(uint8_t auto_on)
{
  /* auto_on == 1 일 때 LED ON (HIGH), 아니면 OFF (LOW) */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                    auto_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

