# STM32CubeMX equivalent: HAL + startup + system + it + msp
# Paths relative to project root (stm32/)

set(STM32CUBEMX_SOURCES
  # Startup (GCC)
  ${CMAKE_SOURCE_DIR}/startup_stm32f401xe.s
  # System
  ${CMAKE_SOURCE_DIR}/Core/Src/system_stm32f4xx.c
  ${CMAKE_SOURCE_DIR}/Core/Src/syscalls.c
  ${CMAKE_SOURCE_DIR}/Core/Src/sysmem.c
  # Core
  ${CMAKE_SOURCE_DIR}/Core/Src/stm32f4xx_it.c
  ${CMAKE_SOURCE_DIR}/Core/Src/stm32f4xx_hal_msp.c
  # HAL (enabled in hal_conf: TIM, UART + dependencies)
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
)

add_library(stm32cubemx STATIC ${STM32CUBEMX_SOURCES})

target_include_directories(stm32cubemx PUBLIC
  ${CMAKE_SOURCE_DIR}/Core/Inc
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
  ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
  ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Include
)

target_compile_definitions(stm32cubemx PUBLIC
  USE_HAL_DRIVER
  STM32F401xE
)

# MCU flags for Cortex-M4F
target_compile_options(stm32cubemx PUBLIC
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
)

target_link_options(stm32cubemx PUBLIC
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
)
