# ARM Cortex-M 크로스 컴파일 툴체인 (arm-none-eabi-gcc)
# 라즈베리 파이/리눅스: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 사용자 지정 경로(ARM_GCC_DIR) 우선, 그 다음 일반 PATH, 마지막으로 Windows 기본 설치 경로 탐색
set(_ARM_GCC_HINTS)
if(DEFINED ENV{ARM_GCC_DIR} AND NOT "$ENV{ARM_GCC_DIR}" STREQUAL "")
    list(APPEND _ARM_GCC_HINTS "$ENV{ARM_GCC_DIR}" "$ENV{ARM_GCC_DIR}/bin")
endif()
list(APPEND _ARM_GCC_HINTS
    "C:/Program Files/Arm GNU Toolchain arm-none-eabi/bin"
    "C:/Program Files (x86)/GNU Arm Embedded Toolchain/bin"
    "C:/Program Files (x86)/GNU Tools Arm Embedded/bin"
)
# STM32CubeCLT 번들 GCC 경로도 자동 탐색
file(GLOB _STM32CLT_GCC_HINTS
    "C:/ST/STM32CubeCLT_*/GNU-tools-for-STM32/bin"
)
list(APPEND _ARM_GCC_HINTS ${_STM32CLT_GCC_HINTS})

find_program(CMAKE_C_COMPILER
    NAMES arm-none-eabi-gcc
    HINTS ${_ARM_GCC_HINTS}
)
find_program(CMAKE_ASM_COMPILER
    NAMES arm-none-eabi-gcc
    HINTS ${_ARM_GCC_HINTS}
)
if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR
        "arm-none-eabi-gcc not found.\n"
        "Linux: sudo apt install gcc-arm-none-eabi\n"
        "Windows: install Arm GNU Toolchain and add <install>/bin to PATH,\n"
        "or set ARM_GCC_DIR environment variable (toolchain root or bin path)."
    )
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
