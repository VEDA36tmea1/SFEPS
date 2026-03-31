## STM32 Laser 펌웨어 기술 정리

### 1. MCU / 전체 개요

- **MCU**: `STM32F401RE` (Nucleo-F401RE 보드 기준)
- **프로젝트 경로**: `hardware/stm32-laser/stm32`
- **주요 기능**
  - TIM1 / TIM2 PWM 으로 **2축 서보 모터 제어** (레이저 방향 제어용)
  - `USART1` + DMA 로 **ESP-8266(WiFi 모듈)** 과 통신 (AT 명령, TCP 브리지)
  - `USART2` 로 **PC 디버그/수동 제어 시리얼** 제공
  - `PC13` 버튼 / `PA5` LED 로 **모드 전환(MANUAL/AUTO)** 표시

> 레이저 자체를 ON/OFF 하는 **전용 GPIO 핀은 현재 코드에는 정의되어 있지 않고**,  
> 대신 두 개의 PWM 채널(PA8, PA0)이 레이저 방향을 제어하는 서보 모터에 매핑되어 있습니다.

---

### 2. 핀 매핑 정리

#### 2.1 서보 모터 (레이저 방향 제어)

타이머/핀 설정은 `stm32_laser.ioc`, `stm32f4xx_hal_msp.c`, `servo_driver.c` 에서 확인됩니다.

- **Y축 (서보 1)**
  - **STM32 핀**: `PA8`
  - **기능**: `TIM1_CH1` (PWM 출력)
  - **Cube 설정**: `S_TIM1_CH1` → PWM Generation CH1  
  - **코드 참조**
    - PWM 초기화: `MX_TIM1_Init()` (`main.c`)
    - 핀 설정: `HAL_TIM_MspPostInit()` → `GPIO_AF1_TIM1` (`stm32f4xx_hal_msp.c`)
    - 제어 API: `Servo_SetCh1Us()`, `Servo_SetAllUs()` (`servo_driver.c`)

- **X축 (서보 2)**
  - **STM32 핀**: `PA0` (WKUP)
  - **기능**: `TIM2_CH1`
  - **Cube 설정**: `S_TIM2_CH1_ETR` → PWM Generation CH1  
  - **코드 참조**
    - PWM 초기화: `MX_TIM2_Init()` (`main.c`)
    - 핀 설정: `HAL_TIM_MspPostInit()` → `GPIO_AF1_TIM2` (`stm32f4xx_hal_msp.c`)
    - 제어 API: `Servo_SetCh2Us()`, `Servo_SetAllUs()` (`servo_driver.c`)

#### 2.2 ESP-8266 (WiFi 모듈) – `USART1`

문서 `STM_DEV.md`, `ESP-8266_check.md`, 코드 `stm32f4xx_hal_msp.c` 기준:

- **전원**
  - `ESP VBUS(5V)` ↔ Nucleo `5V`
  - `ESP GND` ↔ Nucleo `GND`

- **UART 신호선**
  - **ESP TX** ↔ **STM32 RX**
    - STM32 핀: **`PA10`**
    - 기능: `USART1_RX`
    - Nucleo 커넥터: D2
    - 설정: `GPIO_AF7_USART1` (`stm32f4xx_hal_msp.c`)
  - **ESP RX** ↔ **STM32 TX**
    - STM32 핀: **`PA9`**
    - 기능: `USART1_TX`
    - Nucleo 커넥터: D8
    - 설정: `GPIO_AF7_USART1` (`stm32f4xx_hal_msp.c`)

- **DMA (ESP → STM32, RX 전용)**
  - `DMA2_Stream2`, Channel 4 (`USART1_RX`)
  - 방향: `PERIPH_TO_MEMORY`
  - 모드: `NORMAL`
  - PC 쪽 로그는 `wifi_rx_pending` 플래그를 통해 `main()` 루프에서 처리.

#### 2.3 PC 디버그 시리얼 – `USART2`

- **STM32 핀**
  - `PA2` → `USART2_TX` (ST-LINK VCP TX → PC RX)
  - `PA3` → `USART2_RX` (ST-LINK VCP RX → PC TX, `USART_RX_Pin` 매크로)
- **용도**
  - 부트 메시지, 모드 변경 로그, 서보 제어 명령 입력, ESP AT 명령 프록시.

#### 2.4 버튼 / LED / 디버그

- **B1 사용자 버튼 (모드 토글)**
  - 핀: `PC13`
  - 기능: `GPIO_EXTI13` (외부 인터럽트, Falling edge)
  - 역할: 누를 때마다 `MODE_MANUAL` ↔ `MODE_AUTO` 토글 (`button_auto_pending` 플래그).

- **LD2 LED / 레이저 테스트 핀**
  - 핀: `PA5`
  - 기능: GPIO Output push-pull
  - 역할:
    - 펌웨어 관점: AUTO 모드 여부를 표시 (`Led_SetAutoMode()`).
    - **하드웨어 구성**: 실제 레이저 모듈의 전원/Enable 신호를 LD2 와 **같이 묶어서** 연결해 두었기 때문에,  
      PA5 를 토글하면 보드 LED 와 레이저가 함께 ON/OFF 되도록 테스트 중.

---

### 3. 클럭 설정 정리

#### 3.1 SystemClock_Config (메인 클럭 트리)

코드 위치: `Core/Src/main.c` 의 `SystemClock_Config()`.

- **입력 오실레이터**
  - `HSI` 16 MHz 사용 (`RCC_OSCILLATORTYPE_HSI`)
  - `HSE` 외부 크리스탈은 사용하지 않음.

- **PLL 설정**
  - PLL 소스: `HSI(16 MHz)`
  - `PLLM = 16` → PLL 입력: 1 MHz
  - `PLLN = 336` → VCO 출력: 336 MHz
  - `PLLP = 4` → `SYSCLK = 336 / 4 = 84 MHz`
  - `PLLQ = 7` → USB 등 주변용 48 MHz

- **버스 클럭**
  - `AHB` (`HCLK`) = 84 MHz (`DIV1`)
  - `APB1` = 42 MHz (`DIV2`)
  - `APB2` = 84 MHz (`DIV1`)
  - 타이머 클럭:
    - APB1 타이머(TIM2 등): 84 MHz
    - APB2 타이머(TIM1 등): 84 MHz

#### 3.2 PWM 타이머 설정 및 펄스 폭 단위

`MX_TIM1_Init()`, `MX_TIM2_Init()` (둘 다 `main.c`) 에서 설정:

- **TIM1 (PA8, 서보 Y축)**
  - Prescaler = 83
  - Period(초기) = 19999
  - 결과:  
    - 타이머 클럭 = 84 MHz / (83+1) = 1 MHz  
    - 1 카운트 = 1 µs  
    - 20,000 카운트 = 20 ms (50 Hz, 서보 표준)
  - PWM 출력 모드: `TIM_OCMODE_PWM1`, `OCPolarity = HIGH` (서보 일반 규격에 맞게 수정됨).

- **TIM2 (PA0, 서보 X축)**
  - Prescaler = 83 → 동일하게 1 MHz 타이머 클럭
  - Period 는 초기에는 매우 크게(0xFFFFFFFF) 설정된 뒤,  
    메인 코드에서 ARR 를 TIM1 과 동일한 `pwm_period` 로 덮어씀:
    - `__HAL_TIM_SET_AUTORELOAD(&htim1, pwm_period);`
    - `__HAL_TIM_SET_AUTORELOAD(&htim2, pwm_period);`
  - 따라서 두 타이머 모두 **1 tick = 1 µs**,  
    `Servo_Set*Us(1500)` 이라면 **1500 µs 펄스**가 됨.

---

### 4. 주요 모듈 및 함수 동작 설명

#### 4.1 `main.c` – 애플리케이션 엔트리 & 메인 루프

- **전역 상태**
  - `TIM_HandleTypeDef htim1, htim2` : 두 서보 PWM 타이머
  - `UART_HandleTypeDef huart1, huart2` : ESP/PC UART
  - `wifi_rx_dma_buffer[]`, `wifi_rx_pending`, `wifi_rx_len` : ESP → STM32 수신 DMA 버퍼/플래그
  - `rx_line_buf[]`, `rx_idx`, `rx_ready` : PC(USART2) 한 줄 입력 버퍼
  - `control_mode` : `MODE_MANUAL`(0) / `MODE_AUTO`(1)
  - `auto_pwm_val`, `auto_dir`, `auto_last_tick`, `last_uart_tick` : 자동 스윕 상태
  - `wifi_link_ok`, `wifi_connecting`, `wifi_last_check`, `wifi_last_cmd_tick` : WiFi/TCP 상태
  - `wifi_send_pending`, `wifi_send_buf[]`, `wifi_send_len` : PING/PONG용 TCP 송신 상태

- **초기화 흐름 (`main()`)**
  1. `HAL_Init()` → HAL 및 SysTick 초기화
  2. `SystemClock_Config()` → 84 MHz 클럭 설정
  3. `MX_GPIO_Init()` → 버튼/LED 및 기본 GPIO
  4. `MX_DMA_Init()` → (주로 USART2 RX용 DMA1 클럭 & NVIC)
  5. `MX_TIM1_Init()`, `MX_TIM2_Init()` → PWM 타이머 베이스 설정
  6. `MX_USART2_UART_Init()` → PC 디버그 UART
  7. `MX_USART1_UART_Init()` → ESP-8266 UART
  8. 부트 로그를 `huart2` 로 출력
  9. `HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);`  
     + `UART_IT_IDLE` 활성화 → ESP 측 한 줄을 DMA+IDLE 로 수신
  10. PWM 주기를 50 Hz 기준으로 재설정 후, `Servo_Init()` 로 1500us 기본값 설정
  11. `HAL_UART_Receive_IT(&huart2, &rx_byte, 1);` → PC 에서 1바이트씩 인터럽트 수신 시작
  12. `Led_Init()`, `Led_SetAutoMode()` 로 LED 초기 상태 세팅

- **헬퍼 함수**
  - `Wifi_SendLine(const char *line)`
    - 문자열 한 줄을 `USART1`(ESP) 로 전송하고 자동으로 `\r\n` 추가.
  - `Wifi_MaybeReconnect(void)`
    - `wifi_link_ok == 0` 이고 `wifi_connecting == 0` 일 때만  
      5초 간격(`WIFI_RECONNECT_INTERVAL`)으로 `AT+CIPSTART="TCP",WIFI_SERVER_IP,WIFI_SERVER_PORT` 전송.
    - 응답이 일정 시간(`WIFI_CMD_TIMEOUT`) 내에 오지 않으면 실패로 간주하고 다시 재시도 대상.

- **인터럽트 콜백**
  - `HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)`
    - 대상: `huart2` (PC ↔ STM32)
    - 수신한 1바이트를 `rx_line_buf` 에 누적,
    - `\r` 또는 `\n` 이 오거나 버퍼가 가득 차면 `rx_ready = 1` 세팅.
  - `HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)`
    - `PC13(B1)` 인터럽트에서 `button_auto_pending = 1` 세팅 → 메인 루프에서 모드 토글.

- **메인 루프의 핵심 처리**
  1. **버튼 모드 토글**
     - `button_auto_pending` 이 세트되면 `MODE_MANUAL` ↔ `MODE_AUTO` 전환
     - AUTO 진입 시 스윕 파라미터 초기화, LED 상태 갱신.
  2. **디버그 에코**
     - `debug_rx_flag` 가 세트되면 가장 최근 수신 바이트를 USART2 로 에코.
  3. **WiFi(ESP) 수신 처리 (`wifi_rx_pending`)**
     - DMA+IDLE 로 한 줄을 수신하면:
       - ESP 응답을 그대로 PC(USART2)로 에코.
       - 문자열에서 `"PING,"` 를 찾으면 `PONG,<payload>\n` 을 만들어  
         `AT+CIPSEND` → `'>'` 프롬프트 → 실제 데이터 송신 순서로 TCP 응답.
       - 다른 라인의 경우:
         - `CLOSED` / `STATUS:4` → 연결 끊김 상태로 플래그 클리어.
         - `ALREADY CONNECTED`, `CONNECT`, `STATUS:3`, `+IPD`, `SEND OK`, `OK` → 연결 OK 상태로 플래그 세트.
         - `ERROR`, `FAIL` → 이미 연결된 상태면 단순 재요청 실패로만 처리, 연결은 유지.
       - 마지막에 다시 `HAL_UART_Receive_DMA()` 로 DMA 수신 재시작.
  4. **PC 한 줄 명령 처리 (`rx_ready`)**
     - `ESP:` / `esp:` 프리픽스
       - 프리픽스 뒤 문자열을 ESP 로 AT 명령으로 전달 (`Wifi_SendLine()`).
     - `AT` 또는 `at` 로 시작
       - 해당 줄을 그대로 ESP 로 보내고, PC 터미널에도 에코.
     - `mode 0` / `mode 1`
       - `control_mode` 변경 + `Led_SetAutoMode()` 호출.
     - 그 외 → **서보 제어 명령**
       - `"X:1500"` / `"X 1500"` : `Servo_SetCh2Us()` 로 `PA0`(TIM2_CH1)만 변경.
       - `"Y:1500"` / `"Y 1500"` : `Servo_SetCh1Us()` 로 `PA8`(TIM1_CH1)만 변경.
       - `"1500"` / `"1500 1200"` : `Servo_SetAllUs()` 로 두 채널 동시에 설정.
       - 모든 값은 `PWM_US_MIN`~`PWM_US_MAX` 범위로 클램프.
  5. **AUTO 모드 서보 스윕**
     - `control_mode == MODE_AUTO` 일 때만 동작.
     - UART 명령으로 값을 보낸 뒤 `AUTO_HOLD_MS` (기본 2000 ms) 동안은  
       해당 값으로 유지하고 스윕 일시 정지.
     - 그 이후부터 `AUTO_MS` 간격(30 ms)으로  
       `AUTO_PWM_MIN`(1200) ↔ `AUTO_PWM_MAX`(1800) 범위를 왕복하며  
       `Servo_SetAllUs(auto_pwm_val, auto_pwm_val)` 호출.
  6. **WiFi 재접속 시도**
     - 루프 끝에서 항상 `Wifi_MaybeReconnect()` 호출로  
       실제 TCP 연결이 끊긴 경우에만 5초 간격 재접속.

---

#### 4.2 `servo_driver.c / servo_driver.h` – 서보 드라이버

- **상수 정의 (`servo_driver.h`)**
  - `PWM_FREQ_HZ = 50` : 50 Hz, 20 ms 주기
  - `PWM_US_MIN = 800`, `PWM_US_MAX = 2200` : 서보 펄스 허용 범위

- **내부 헬퍼**
  - `static uint32_t clamp_us(uint32_t us)`
    - 인자로 받은 펄스 폭을 `[PWM_US_MIN, PWM_US_MAX]` 범위로 제한.

- **공개 API**
  - `Servo_Init(void)`
    - TIM1_CH1, TIM2_CH1 의 캡처/비교 레지스터를 1500 us 로 초기화.
    - `HAL_TIM_PWM_Start()` 로 두 타이머의 PWM 채널 시작.
    - TIM1 의 MOE(Main Output Enable) 비트를 켜서 실제 핀 출력 활성화.
  - `Servo_SetAllUs(uint32_t us_ch1, uint32_t us_ch2)`
    - 두 인자를 각각 클램프 후
    - `__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, us_ch1);`
    - `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, us_ch2);`
  - `Servo_SetCh1Us(uint32_t us)`
    - Y축( PA8 / TIM1_CH1 )만 변경.
  - `Servo_SetCh2Us(uint32_t us)`
    - X축( PA0 / TIM2_CH1 )만 변경.

---

#### 4.3 `led_driver.c / led_driver.h` – LED 드라이버

- `Led_Init(void)`
  - LD2 핀(`PA5`)을 명시적으로 `RESET` 상태로 초기화.
  - 실제 GPIO 모드/클럭 설정은 `MX_GPIO_Init()` 에서 수행.

- `Led_SetAutoMode(uint8_t auto_on)`
  - `auto_on == 1` → `LD2` HIGH (AUTO 모드 ON)
  - `auto_on == 0` → `LD2` LOW  (MANUAL 모드 또는 비활성)

---

#### 4.4 `stm32f4xx_hal_msp.c` – 저수준 MSP 초기화 (핵심 부분만)

- **HAL_MspInit()**
  - `SYSCFG`, `PWR` 클럭 활성화
  - NVIC 우선순위 그룹 설정 (`NVIC_PRIORITYGROUP_0`)

- **HAL_TIM_Base_MspInit() / HAL_TIM_Base_MspDeInit()**
  - `TIM1`, `TIM2` 의 클럭 Enable/Disable 만 수행.

- **HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim)**
  - `TIM1` → `PA8` 를 `GPIO_AF1_TIM1` 으로 설정.
  - `TIM2` → `PA0` 를 `GPIO_AF1_TIM2` 으로 설정.

- **HAL_UART_MspInit(UART_HandleTypeDef *huart)**
  - `USART1` (ESP)
    - 클럭 Enable, `PA9/PA10` 을 `AF7` 로 설정.
    - `DMA2_Stream2` Channel 4 를 `PERIPH_TO_MEMORY`, BYTE 정렬, NORMAL 모드, HIGH 우선순위로 초기화.
    - `__HAL_LINKDMA(huart, hdmarx, hdma_usart1_rx);`
    - `USART1_IRQn` NVIC Enable.
  - `USART2` (PC)
    - 클럭 Enable, `PA2/PA3` 을 `AF7` 로 설정.
    - `DMA1_Stream5` Channel 4 를 RX 용으로 설정 (현재 프로젝트에서는 주로 IT 기반 RX 사용).
    - `USART2_IRQn` NVIC Enable.

---

#### 4.5 `stm32f4xx_it.c` – 인터럽트 핸들러

- **DMA1_Stream5_IRQHandler**
  - `HAL_DMA_IRQHandler(&hdma_usart2_rx);` 호출 (USART2 RX DMA).

- **DMA2_Stream2_IRQHandler**
  - `HAL_DMA_IRQHandler(&hdma_usart1_rx);` 호출 (USART1 RX DMA).

- **USART1_IRQHandler (WiFi ESP-8266)**
  - IDLE 라인 플래그가 세트되면:
    - DMA 카운터에서 실제 수신된 바이트 수 계산 → `wifi_rx_len`.
    - `HAL_UART_AbortReceive()` 로 DMA 중단.
    - `wifi_rx_pending = 1;` 세트 → 메인 루프가 한 줄 처리.
  - 마지막에 `HAL_UART_IRQHandler(&huart1);` 로 HAL 내부 처리.

- **USART2_IRQHandler**
  - `HAL_UART_IRQHandler(&huart2);` 호출 → RX 완료 시 `HAL_UART_RxCpltCallback()` 실행.

- **EXTI15_10_IRQHandler**
  - `HAL_GPIO_EXTI_IRQHandler(B1_Pin);` → 이후 `HAL_GPIO_EXTI_Callback()`에서 `button_auto_pending = 1`.

---

### 5. 레이저 Signal 핀에 대한 현재 상태

- 현재 CubeMX 설정(`stm32_laser.ioc`)과 코드(`main.h`, `MX_GPIO_Init()`, `stm32f4xx_hal_msp.c`) 기준으로는  
  **레이저 전용으로 이름 붙은 GPIO 매크로(`LASER_EN_Pin` 등)는 따로 정의되어 있지 않습니다.**
- 대신, **테스트 단계에서는 레이저 전원/Enable 신호를 LD2(보드 LED)와 같은 핀 `PA5` 에 묶어서 사용**하고 있습니다.
  - 즉, `Led_SetAutoMode()` 로 PA5 를 제어하면,
    - 보드의 녹색 LED 와 레이저 모듈이 **동시에 ON/OFF** 됩니다.
  - 이 구성은 “LED 상태 = 레이저 상태” 로 간단히 눈으로 확인하기 위한 임시 테스트용입니다.
- 방향 제어는 기존과 동일하게:
  - **PA8 / PA0 PWM 으로 구동되는 2축 서보**를 통해 거울/모터를 움직여 레이저 방향을 조정합니다.
- 추후 레이저 전원을 LED 와 분리하고 싶다면:
  - 예: `PBx` 나 여유 GPIO 를 `GPIO_Output` 으로 CubeMX 에 추가하고,
  - `main.h` 에 `LASER_EN_Pin`, `LASER_EN_GPIO_Port` 를 정의,
  - `MX_GPIO_Init()` 에서 출력으로 설정하고, 별도 제어 함수(예: `Laser_SetEnabled()`) 로 분리 제어할 수 있습니다.

