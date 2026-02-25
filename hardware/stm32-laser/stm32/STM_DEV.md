## STM32 Laser 펌웨어 개발 로그

### 2026-02-24 – WiFi(ESP-8266) 연동 & DMA 수신 추가

#### 1. 개요

- Nucleo-F401RE 보드에 ESP-8266을 연결해서 **WiFi → ESP-8266 → UART → STM32** 경로로
  기존의 서보 제어 명령(`1500`, `1500 1200`, `mode 0/1`)을 받아서 실행할 수 있도록 수정.
- 기존 **모터(PWM TIM1/TIM2)**, **LED(PA5 D13)** 제어 로직은 그대로 유지하고,
  **명령 입력 경로만 확장**.

#### 2. 하드웨어 연결 (ESP-8266 ↔ Nucleo-F401RE)

- USART1 (WiFi용)를 사용.

| ESP-8266 핀 | Nucleo 핀 | 설명                    |
|-------------|-----------|-------------------------|
| VBUS (5V)   | 5V        | 전원                    |
| GND         | GND       | 기준 GND                |
| **TX**      | **D2 (PA10)** | STM32 USART1_RX (수신) |
| **RX**      | **D8 (PA9)**  | STM32 USART1_TX (송신) |

#### 3. USART1 (WiFi용) 설정

- 인스턴스: `USART1`  
- 핀:
  - `PA9`  → `USART1_TX` (Nucleo D8, ESP-8266 RX)
  - `PA10` → `USART1_RX` (Nucleo D2, ESP-8266 TX)
- 속도: `115200 8N1`

추가/변경 파일:

- `stm32/Core/Inc/main.h`
  - `USART1_TX_Pin`, `USART1_TX_GPIO_Port`
  - `USART1_RX_Pin`, `USART1_RX_GPIO_Port` 매크로 추가.

- `stm32/Core/Src/main.c`
  - `UART_HandleTypeDef huart1;` 추가 (WiFi용).
  - `DMA_HandleTypeDef hdma_usart1_rx;` 추가.
  - WiFi DMA 버퍼/플래그:
    - `#define WIFI_RX_DMA_SIZE 256`
    - `static uint8_t wifi_rx_dma_buffer[WIFI_RX_DMA_SIZE];`
    - `volatile uint8_t wifi_rx_pending;`
    - `volatile uint16_t wifi_rx_len;`
  - `MX_USART1_UART_Init()` 추가:
    - `USART1`를 115200 8N1, TX/RX 활성화로 초기화.
  - `MX_DMA_Init()`에서 `DMA2` 클럭 및 `DMA2_Stream2_IRQn` NVIC 활성화.
  - `main()` 초기화 순서:
    - `MX_USART2_UART_Init();` (기존 PC UART)
    - `MX_USART1_UART_Init();` (WiFi)
    - 이후:
      - `HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);`
      - `__HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);` 로 IDLE 라인 인터럽트 활성화.

#### 4. DMA2 (USART1_RX) 설정

- `stm32/Core/Src/stm32f4xx_hal_msp.c`

  - `extern DMA_HandleTypeDef hdma_usart1_rx;` 추가.
  - `HAL_UART_MspInit()` 안에 `USART1` 분기 추가:
    - 클럭:
      - `__HAL_RCC_USART1_CLK_ENABLE();`
      - `__HAL_RCC_GPIOA_CLK_ENABLE();`
    - GPIO:
      - `PA9`  AF7, Push-Pull, High speed → `USART1_TX_Pin`
      - `PA10` AF7, Push-Pull, High speed → `USART1_RX_Pin`
    - DMA:
      - `DMA2_Stream2`
      - `Channel 4` (USART1_RX)
      - 방향: `PERIPH_TO_MEMORY`
      - 정렬: `BYTE` / `BYTE`
      - 모드: `DMA_NORMAL` (Normal 모드)
      - 우선순위: HIGH
      - `__HAL_LINKDMA(huart, hdmarx, hdma_usart1_rx);`
    - NVIC:
      - `USART1_IRQn` 우선순위 0,0, Enable

  - `HAL_UART_MspDeInit()` 안에 `USART1` 분기 추가:
    - USART1 클럭 Disable
    - PA9/PA10 DeInit
    - DMA RX DeInit
    - `USART1_IRQn` Disable

- `stm32/Core/Src/main.c` 의 `MX_DMA_Init()`:
  - `__HAL_RCC_DMA2_CLK_ENABLE();`
  - `DMA2_Stream2_IRQn` 우선순위 0,0, Enable.

#### 5. IDLE 라인 + DMA 기반 수신 플로우

- **목표**: WiFi(USART1)에서 들어오는 한 줄(`\r`/`\n` 포함)을 DMA로 받아서,
  기존 UART 파서(`rx_line_buf`)로 그대로 처리.

1. `HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);`
2. `USART1`에서 데이터 수신.
3. 아무 데이터 없이 일정 시간 지나 IDLE 라인 발생 → `USART1_IRQHandler`에서 처리:
   - `UART_FLAG_IDLE` 확인 후:
     - `__HAL_UART_CLEAR_IDLEFLAG(&huart1);`
     - `uint16_t n = __HAL_DMA_GET_COUNTER(huart1.hdmarx);`
     - `wifi_rx_len = WIFI_RX_DMA_SIZE - n;`
     - `HAL_UART_AbortReceive(&huart1);` 로 DMA 수신 중단
     - `wifi_rx_pending = 1;`
   - 마지막에 `HAL_UART_IRQHandler(&huart1);` 호출.
4. `main()` 루프에서:

   ```c
   if (wifi_rx_pending) {
     wifi_rx_pending = 0;
     uint16_t len = wifi_rx_len;
     if (len > RX_LINE_MAX - 1) len = RX_LINE_MAX - 1;
     // DMA 버퍼 → 기존 rx_line_buf 복사
     for (uint16_t i = 0; i < len; i++)
       rx_line_buf[i] = (char)wifi_rx_dma_buffer[i];
     rx_idx = len;
     rx_ready = 1;
     HAL_UART_Receive_DMA(&huart1, wifi_rx_dma_buffer, WIFI_RX_DMA_SIZE);
   }
   ```

5. 이후 `rx_ready` 처리 로직은 기존과 동일:
   - `"mode 0"` / `"mode 1"` → `control_mode` 변경 + LED 상태 반영.
   - `"1500"` / `"1500 1200"` → TIM1/TIM2 PWM compare 값 갱신.

#### 6. 명령/응답 경로 변경 정리

- **입력 (명령)**:
  - 기존: 주로 `USART2`(PA2/PA3)에서 PC 터미널로 입력.
  - 추가: **`USART1`(WiFi)** 에서 ESP-8266 → STM32로 명령 입력.
  - 현재 구현에서는 **WiFi(USART1)를 주 명령 입력 경로**로 사용하도록 응답도 USART1에 맞춤.

- **출력 (응답/로그)**:
  - 기존: `huart2` 로 `"OK ..."`, `"mode ..."`, 에러 메시지 전송.
  - 변경: 위 응답 문자열은 **모두 `huart1`(WiFi)** 로 전송하도록 변경.
  - `huart2`는 **1바이트 에코/디버그용**으로 남겨둠:
    - `debug_rx_flag` 세팅 시 `HAL_UART_Transmit(&huart2, &debug_rx_byte, 1, 20);`

- **모터/LED 제어 영향**:
  - TIM1/TIM2 PWM 설정, 자동 스윕 로직, `PA5_D13_SetByMode()` 등의
    **모터·LED 제어 로직은 변경 없음**.
  - 명령이 어디서 들어오든(PC UART / WiFi) **최종적으로 동일한 파서를 거쳐
    같은 PWM 값/모드 변경**을 수행.

#### 7. 인터럽트 핸들러 추가

- `stm32/Core/Src/stm32f4xx_it.c`

  - 외부 심볼 선언:

    ```c
    extern DMA_HandleTypeDef hdma_usart1_rx;
    extern UART_HandleTypeDef huart1;
    extern volatile uint8_t wifi_rx_pending;
    extern volatile uint16_t wifi_rx_len;
    #define WIFI_RX_DMA_SIZE 256
    ```

  - `USART1_IRQHandler`:

    ```c
    void USART1_IRQHandler(void)
    {
      if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE))
      {
        __HAL_UART_CLEAR_IDLEFLAG(&huart1);
        uint16_t n = (uint16_t)__HAL_DMA_GET_COUNTER(huart1.hdmarx);
        wifi_rx_len = WIFI_RX_DMA_SIZE - n;
        (void)HAL_UART_AbortReceive(&huart1);
        wifi_rx_pending = 1;
      }
      HAL_UART_IRQHandler(&huart1);
    }
    ```

  - `DMA2_Stream2_IRQHandler`:

    ```c
    void DMA2_Stream2_IRQHandler(void)
    {
      HAL_DMA_IRQHandler(&hdma_usart1_rx);
    }
    ```

---

이 문서는 **STM32 펌웨어 쪽 변경 이력/구조를 빠르게 복기**하기 위한 용도로 유지한다.
앞으로 날짜별로 주요 변경사항이 생기면 같은 형식으로 추가하면 된다.

---

### 2026-02-24 – 서보/LED 드라이버 분리 (servo_driver, led_driver)

#### 1. 개요

- `Core/Src/main.c` 에 몰려 있던 **서보 PWM(TIM1/TIM2)**, **LD2 LED(PA5)** 제어 코드를
  별도 드라이버 모듈로 분리해서 가독성과 재사용성을 높임.
- 상위 로직(main 루프, 명령 파서)은 **“각도(us)만 넘기는 수준의 API”** 를 사용하도록 변경.

#### 2. 추가된 파일

- `Core/Inc/servo_driver.h`
- `Core/Src/servo_driver.c`
- `Core/Inc/led_driver.h`
- `Core/Src/led_driver.c`

#### 3. `servo_driver` 설계

- **헤더**: `Core/Inc/servo_driver.h`

  - 공통 파라미터 정의:

    ```c
    #define PWM_FREQ_HZ   50    /* 50Hz, 20ms 주기 */
    #define PWM_US_MIN    800
    #define PWM_US_MAX    2200
    ```

  - API:

    ```c
    void Servo_Init(void);
    void Servo_SetAllUs(uint32_t us_ch1, uint32_t us_ch2);
    void Servo_SetCh1Us(uint32_t us);
    void Servo_SetCh2Us(uint32_t us);
    ```

- **소스**: `Core/Src/servo_driver.c`

  - `extern TIM_HandleTypeDef htim1, htim2;` 로 타이머 핸들 참조.
  - 내부 `clamp_us()` 로 `PWM_US_MIN`~`PWM_US_MAX` 범위 클램프.
  - `Servo_Init()`:
    - CH1(파8=TIM1_CH1), CH2(파0=TIM2_CH1)를 1500us로 초기화.
    - `HAL_TIM_PWM_Start` / `__HAL_TIM_MOE_ENABLE(&htim1)` 호출.
  - `Servo_SetAllUs()`:
    - 두 채널 펄스를 us 단위로 받아서 클램프 후 `__HAL_TIM_SET_COMPARE` 로 설정.

#### 4. `led_driver` 설계

- **헤더**: `Core/Inc/led_driver.h`

  - API:

    ```c
    void Led_Init(void);
    void Led_SetAutoMode(uint8_t auto_on);
    ```

- **소스**: `Core/Src/led_driver.c`

  - `Led_Init()`:
    - LD2 핀을 명시적으로 `RESET` 상태로 둠
    - 실제 GPIO 모드는 여전히 `MX_GPIO_Init()` 에서 설정.
  - `Led_SetAutoMode(uint8_t auto_on)`:
    - `auto_on == 1` → `GPIO_PIN_SET` (LED ON)
    - `auto_on == 0` → `GPIO_PIN_RESET` (LED OFF)
  - 기존 `PA5_D13_SetByMode()` 로직을 대체.

#### 5. `main.c` 변경 요약

- 상단 include 추가:

  ```c
  #include "servo_driver.h"
  #include "led_driver.h"
  ```

- **전역 define 정리**:
  - `PWM_FREQ_HZ`, `PWM_US_MIN`, `PWM_US_MAX` 는 `servo_driver.h` 로 이동.
  - `main.c` 에서는 `RX_LINE_MAX` 만 유지:

    ```c
    #define RX_LINE_MAX   64
    ```

- **초기화 부분**:

  ```c
  {
    uint32_t pwm_period = (1000000u / (uint32_t)PWM_FREQ_HZ) - 1u;
    __HAL_TIM_SET_AUTORELOAD(&htim1, pwm_period);
    __HAL_TIM_SET_AUTORELOAD(&htim2, pwm_period);
  }
  Servo_Init();
  HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
  auto_last_tick = HAL_GetTick();
  Led_Init();
  Led_SetAutoMode(control_mode == MODE_AUTO);
  ```

- **모드 전환 / 명령 처리 시 LED 제어**:

  - 버튼 / `"mode 0/1"` 명령 처리 후:

    ```c
    control_mode = MODE_MANUAL 또는 MODE_AUTO;
    Led_SetAutoMode(control_mode == MODE_AUTO);
    ```

- **서보 펄스 설정 (명령 파서 부분)**:

  ```c
  // u1, u2 계산 후
  Servo_SetAllUs((uint32_t)u1, (uint32_t)u2);
  auto_pwm_val = (uint32_t)u1;
  ```

- **AUTO 스윕 시 서보 제어**:

  ```c
  Servo_SetAllUs(auto_pwm_val, auto_pwm_val);
  ```

#### 6. CMake 반영

- `stm32/CMakeLists.txt` 에 드라이버 소스 추가:

  ```cmake
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE
      Core/Src/main.c
      Core/Src/servo_driver.c
      Core/Src/led_driver.c
  )
  ```

- 빌드:

  ```bash
  cd hardware/stm32-laser/stm32
  cmake --build build/Debug -j
  ```

  성공적으로 `stm32_laser` ELF 및 `.bin` 생성 확인.

