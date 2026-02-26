# ARM Cortex-M 크로스 컴파일 툴체인 (arm-none-eabi-gcc)
# 라즈베리 파이/리눅스: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

find_program(CMAKE_C_COMPILER NAMES arm-none-eabi-gcc)
find_program(CMAKE_ASM_COMPILER NAMES arm-none-eabi-gcc)
if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR "arm-none-eabi-gcc not found. Install: sudo apt install gcc-arm-none-eabi")
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
