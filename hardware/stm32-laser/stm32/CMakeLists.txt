cmake_minimum_required(VERSION 3.18)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm32_laser)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
# C++를 사용하지 않으므로 CXX 탐색을 막아 Windows 환경 오류를 줄인다.
project(${CMAKE_PROJECT_NAME} LANGUAGES C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})


# Enable CMake support for ASM and C languages
enable_language(C ASM)

# ------------------------------------------------------------
# MCU / ABI flags (C/ASM 공통)
# - Cortex-M4는 Thumb 전용이므로 -mthumb 가 반드시 필요
# - 이 플래그는 "각 타깃"에 명시적으로 걸어야 하며,
#   특히 subdir의 OBJECT 라이브러리에는 상위 타깃 옵션이 자동 전파되지 않음
# ------------------------------------------------------------
set(STM32_CPU_FLAGS
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES SUFFIX ".elf")
# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/main.c
    Core/Src/servo_driver.c
    Core/Src/led_driver.c
    Core/Src/ESP_Parser.c
    Core/Src/UART_Parser.c
)

# Linker script
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T${CMAKE_SOURCE_DIR}/STM32F401XX_FLASH.ld
)

# MCU flags (C/ASM 모두 적용)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE ${STM32_CPU_FLAGS})
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE ${STM32_CPU_FLAGS})

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32F401xE
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Link order: stm32cubemx first (HAL, startup), then C lib, then stm32cubemx again
# so that newlib syscalls (_write, _sbrk, _exit, ...) are resolved from syscalls.c/sysmem.c
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    stm32cubemx
    -lc -lm -lgcc
    stm32cubemx
)

# Linker script for STM32F401RE (defines _estack, _sdata, _sbss, etc.)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32F401XX_FLASH.ld)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${CMAKE_PROJECT_NAME}.map
    -Wl,--gc-sections
)

# Generate .bin from .elf after build (for st-flash write)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/$<TARGET_FILE_BASE_NAME:${CMAKE_PROJECT_NAME}>.bin
    COMMENT "Generating .bin from .elf"
)
